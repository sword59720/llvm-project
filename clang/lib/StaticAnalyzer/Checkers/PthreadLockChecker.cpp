//===--- PthreadLockChecker.cpp - Check for locking problems ---*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines:
//  * PthreadLockChecker, a simple lock -> unlock checker.
//    Which also checks for XNU locks, which behave similarly enough to share
//    code.
//  * FuchsiaLocksChecker, which is also rather similar.
//  * C11LockChecker which also closely follows Pthread semantics.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ParentMapContext.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallDescription.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerHelpers.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/JSON.h"

using namespace clang;
using namespace ento;

constexpr llvm::StringRef LOCK_CHECKER_CATEGORY = "Lock checker";

static bool isLockRelevant(const MemRegion *R,
                           const PathSensitiveBugReport &BR) {
  return BR.getBugType().getCategory() == LOCK_CHECKER_CATEGORY &&
         BR.isInteresting(R);
}

static const NoteTag *createMutexNote(CheckerContext &C, const MemRegion *R,
                                      StringRef MsgForNamed,
                                      StringRef MsgForUnnamed) {
  return C.getNoteTag(
      [R, Named = MsgForNamed.str(), Unnamed = MsgForUnnamed.str()](
          PathSensitiveBugReport &BR, llvm::raw_ostream &OS) {
        if (!isLockRelevant(R, BR))
          return;
        std::string Name = R->getDescriptiveName();
        if (Name.empty())
          OS << Unnamed;
        else
          OS << Named << Name << " here";
      });
}

namespace {

struct LockState {
  enum Kind {
    Destroyed,
    Locked,
    Unlocked,
    UntouchedAndPossiblyDestroyed,
    UnlockedAndPossiblyDestroyed
  } K;

private:
  LockState(Kind K) : K(K) {}

public:
  static LockState getLocked() { return LockState(Locked); }
  static LockState getUnlocked() { return LockState(Unlocked); }
  static LockState getDestroyed() { return LockState(Destroyed); }
  static LockState getUntouchedAndPossiblyDestroyed() {
    return LockState(UntouchedAndPossiblyDestroyed);
  }
  static LockState getUnlockedAndPossiblyDestroyed() {
    return LockState(UnlockedAndPossiblyDestroyed);
  }

  bool operator==(const LockState &X) const { return K == X.K; }

  bool isLocked() const { return K == Locked; }
  bool isUnlocked() const { return K == Unlocked; }
  bool isDestroyed() const { return K == Destroyed; }
  bool isUntouchedAndPossiblyDestroyed() const {
    return K == UntouchedAndPossiblyDestroyed;
  }
  bool isUnlockedAndPossiblyDestroyed() const {
    return K == UnlockedAndPossiblyDestroyed;
  }

  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddInteger(K); }
};

class PthreadLockChecker : public Checker<check::PostCall, check::DeadSymbols,
                                          check::RegionChanges, check::Bind,
                                          check::BeginFunction, check::PreStmt<GCCAsmStmt>> {
public:
  enum LockingSemantics { NotApplicable = 0, PthreadSemantics, XNUSemantics };
  enum CheckerKind {
    CK_PthreadLockChecker,
    CK_FuchsiaLockChecker,
    CK_C11LockChecker,
    CK_NumCheckKinds
  };
  bool ChecksEnabled[CK_NumCheckKinds] = {false};
  CheckerNameRef CheckNames[CK_NumCheckKinds];
  bool WarnOnLockOrderReversal = false;

private:
  typedef void (PthreadLockChecker::*FnCheck)(const CallEvent &Call,
                                              CheckerContext &C,
                                              CheckerKind CheckKind) const;
  CallDescriptionMap<FnCheck> PThreadCallbacks = {
      // Init.
      {{CDM::CLibrary, {"pthread_mutex_init"}, 2},
       &PthreadLockChecker::InitAnyLock},
      // TODO: pthread_rwlock_init(2 arguments).
      // TODO: lck_mtx_init(3 arguments).
      // TODO: lck_mtx_alloc_init(2 arguments) => returns the mutex.
      // TODO: lck_rw_init(3 arguments).
      // TODO: lck_rw_alloc_init(2 arguments) => returns the mutex.

      // Acquire.
      {{CDM::CLibrary, {"pthread_mutex_lock"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},
      {{CDM::CLibrary, {"pthread_rwlock_rdlock"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},
      {{CDM::CLibrary, {"pthread_rwlock_wrlock"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},
      {{CDM::CLibrary, {"lck_mtx_lock"}, 1},
       &PthreadLockChecker::AcquireXNULock},
      {{CDM::CLibrary, {"lck_rw_lock_exclusive"}, 1},
       &PthreadLockChecker::AcquireXNULock},
      {{CDM::CLibrary, {"lck_rw_lock_shared"}, 1},
       &PthreadLockChecker::AcquireXNULock},

      // Try.
      {{CDM::CLibrary, {"pthread_mutex_trylock"}, 1},
       &PthreadLockChecker::TryPthreadLock},
      {{CDM::CLibrary, {"pthread_rwlock_tryrdlock"}, 1},
       &PthreadLockChecker::TryPthreadLock},
      {{CDM::CLibrary, {"pthread_rwlock_trywrlock"}, 1},
       &PthreadLockChecker::TryPthreadLock},
      {{CDM::CLibrary, {"lck_mtx_try_lock"}, 1},
       &PthreadLockChecker::TryXNULock},
      {{CDM::CLibrary, {"lck_rw_try_lock_exclusive"}, 1},
       &PthreadLockChecker::TryXNULock},
      {{CDM::CLibrary, {"lck_rw_try_lock_shared"}, 1},
       &PthreadLockChecker::TryXNULock},

      // Release.
      {{CDM::CLibrary, {"pthread_mutex_unlock"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"pthread_rwlock_unlock"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"lck_mtx_unlock"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"lck_rw_unlock_exclusive"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"lck_rw_unlock_shared"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"lck_rw_done"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},

      // Destroy.
      {{CDM::CLibrary, {"pthread_mutex_destroy"}, 1},
       &PthreadLockChecker::DestroyPthreadLock},
      {{CDM::CLibrary, {"lck_mtx_destroy"}, 2},
       &PthreadLockChecker::DestroyXNULock},
      // TODO: pthread_rwlock_destroy(1 argument).
      // TODO: lck_rw_destroy(2 arguments).
  };

  CallDescriptionMap<FnCheck> FuchsiaCallbacks = {
      // Init.
      {{CDM::CLibrary, {"spin_lock_init"}, 1},
       &PthreadLockChecker::InitAnyLock},

      // Acquire.
      {{CDM::CLibrary, {"spin_lock"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},
      {{CDM::CLibrary, {"spin_lock_save"}, 3},
       &PthreadLockChecker::AcquirePthreadLock},
      {{CDM::CLibrary, {"sync_mutex_lock"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},
      {{CDM::CLibrary, {"sync_mutex_lock_with_waiter"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},

      // Try.
      {{CDM::CLibrary, {"spin_trylock"}, 1},
       &PthreadLockChecker::TryFuchsiaLock},
      {{CDM::CLibrary, {"sync_mutex_trylock"}, 1},
       &PthreadLockChecker::TryFuchsiaLock},
      {{CDM::CLibrary, {"sync_mutex_timedlock"}, 2},
       &PthreadLockChecker::TryFuchsiaLock},

      // Release.
      {{CDM::CLibrary, {"spin_unlock"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"spin_unlock_restore"}, 3},
       &PthreadLockChecker::ReleaseAnyLock},
      {{CDM::CLibrary, {"sync_mutex_unlock"}, 1},
       &PthreadLockChecker::ReleaseAnyLock},
  };

  CallDescriptionMap<FnCheck> C11Callbacks = {
      // Init.
      {{CDM::CLibrary, {"mtx_init"}, 2}, &PthreadLockChecker::InitAnyLock},

      // Acquire.
      {{CDM::CLibrary, {"mtx_lock"}, 1},
       &PthreadLockChecker::AcquirePthreadLock},

      // Try.
      {{CDM::CLibrary, {"mtx_trylock"}, 1}, &PthreadLockChecker::TryC11Lock},
      {{CDM::CLibrary, {"mtx_timedlock"}, 2}, &PthreadLockChecker::TryC11Lock},

      // Release.
      {{CDM::CLibrary, {"mtx_unlock"}, 1}, &PthreadLockChecker::ReleaseAnyLock},

      // Destroy
      {{CDM::CLibrary, {"mtx_destroy"}, 1},
       &PthreadLockChecker::DestroyPthreadLock},
  };

  ProgramStateRef resolvePossiblyDestroyedMutex(ProgramStateRef state,
                                                const MemRegion *lockR,
                                                const SymbolRef *sym) const;
  void reportBug(CheckerContext &C, std::unique_ptr<BugType> BT[],
                 const Expr *MtxExpr, const MemRegion *MtxRegion,
                 CheckerKind CheckKind, StringRef Desc,
                 const MemRegion *ExtraInteresting = nullptr) const;

  // Init.
  void InitAnyLock(const CallEvent &Call, CheckerContext &C,
                   CheckerKind CheckKind) const;
  void InitLockAux(const CallEvent &Call, CheckerContext &C,
                   const Expr *MtxExpr, SVal MtxVal,
                   CheckerKind CheckKind) const;

  // Lock, Try-lock.
  void AcquirePthreadLock(const CallEvent &Call, CheckerContext &C,
                          CheckerKind CheckKind) const;
  void AcquireXNULock(const CallEvent &Call, CheckerContext &C,
                      CheckerKind CheckKind) const;
  void TryPthreadLock(const CallEvent &Call, CheckerContext &C,
                      CheckerKind CheckKind) const;
  void TryXNULock(const CallEvent &Call, CheckerContext &C,
                  CheckerKind CheckKind) const;
  void TryFuchsiaLock(const CallEvent &Call, CheckerContext &C,
                      CheckerKind CheckKind) const;
  void TryC11Lock(const CallEvent &Call, CheckerContext &C,
                  CheckerKind CheckKind) const;
  void AcquireLockAux(const CallEvent &Call, CheckerContext &C,
                      const Expr *MtxExpr, SVal MtxVal, bool IsTryLock,
                      LockingSemantics Semantics, CheckerKind CheckKind) const;

  // Release.
  void ReleaseAnyLock(const CallEvent &Call, CheckerContext &C,
                      CheckerKind CheckKind) const;
  void ReleaseLockAux(const CallEvent &Call, CheckerContext &C,
                      const Expr *MtxExpr, SVal MtxVal,
                      CheckerKind CheckKind) const;

  // Destroy.
  void DestroyPthreadLock(const CallEvent &Call, CheckerContext &C,
                          CheckerKind CheckKind) const;
  void DestroyXNULock(const CallEvent &Call, CheckerContext &C,
                      CheckerKind CheckKind) const;
  void DestroyLockAux(const CallEvent &Call, CheckerContext &C,
                      const Expr *MtxExpr, SVal MtxVal,
                      LockingSemantics Semantics, CheckerKind CheckKind) const;

public:
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBind(SVal Location, SVal, const Stmt *, bool, CheckerContext &C) const;
  void checkBeginFunction(CheckerContext &C) const;
  void checkPreStmt(const GCCAsmStmt *Assembly, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SymReaper, CheckerContext &C) const;
  ProgramStateRef
  checkRegionChanges(ProgramStateRef State, const InvalidatedSymbols *Symbols,
                     ArrayRef<const MemRegion *> ExplicitRegions,
                     ArrayRef<const MemRegion *> Regions, const StackFrame *SF,
                     const CallEvent *Call) const;
  void printState(raw_ostream &Out, ProgramStateRef State, const char *NL,
                  const char *Sep) const override;

private:
  mutable std::unique_ptr<BugType> BT_doublelock[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_selfdeadlock[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_doubleunlock[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_destroylock[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_initlock[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_lor[CK_NumCheckKinds];

  void initBugType(CheckerKind CheckKind) const {
    if (BT_doublelock[CheckKind])
      return;
    BT_doublelock[CheckKind].reset(new BugType{
        CheckNames[CheckKind], "Double locking", LOCK_CHECKER_CATEGORY});
    BT_selfdeadlock[CheckKind].reset(new BugType{
        CheckNames[CheckKind], "Mutex self-deadlock", LOCK_CHECKER_CATEGORY});
    BT_doubleunlock[CheckKind].reset(new BugType{
        CheckNames[CheckKind], "Double unlocking", LOCK_CHECKER_CATEGORY});
    BT_destroylock[CheckKind].reset(new BugType{
        CheckNames[CheckKind], "Use destroyed lock", LOCK_CHECKER_CATEGORY});
    BT_initlock[CheckKind].reset(new BugType{
        CheckNames[CheckKind], "Init invalid lock", LOCK_CHECKER_CATEGORY});
    BT_lor[CheckKind].reset(new BugType{
        CheckNames[CheckKind], "Lock order reversal", LOCK_CHECKER_CATEGORY});
  }
};
} // end anonymous namespace

// A stack of locks for tracking lock-unlock order.
REGISTER_LIST_WITH_PROGRAMSTATE(LockSet, const MemRegion *)

// An entry for tracking lock states.
REGISTER_MAP_WITH_PROGRAMSTATE(LockMap, const MemRegion *, LockState)

// Return values for unresolved calls to pthread_mutex_destroy().
REGISTER_MAP_WITH_PROGRAMSTATE(DestroyRetVal, const MemRegion *, SymbolRef)
using KBLockCallKey = std::pair<const Expr *, const StackFrame *>;
REGISTER_SET_WITH_PROGRAMSTATE(KBLockPreservingCalls, KBLockCallKey)
using KBPreservedRegions = std::pair<const MemRegion *, const MemRegion *>;
REGISTER_MAP_WITH_PROGRAMSTATE(KBScopedLockPreservation, KBLockCallKey,
                               KBPreservedRegions)

enum class KBMutexType { Normal, Recursive, ErrorCheck };
REGISTER_MAP_WITH_PROGRAMSTATE(KBMutexAttributes, const MemRegion *, KBMutexType)
REGISTER_MAP_WITH_PROGRAMSTATE(KBMutexTypes, const MemRegion *, KBMutexType)
REGISTER_MAP_WITH_PROGRAMSTATE(KBMutexDepths, const MemRegion *, unsigned)
REGISTER_MAP_WITH_PROGRAMSTATE(KBLockOrigins, const MemRegion *, const Expr *)

namespace {
struct LiveThread {
  const FunctionDecl *Entry;
  const Expr *Spawn;
  const StackFrame *Frame;
  SVal Handle;
  bool Joinable;
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddPointer(Entry);
    ID.AddPointer(Spawn);
    ID.AddPointer(Frame);
    Handle.Profile(ID);
    ID.AddBoolean(Joinable);
  }
  bool operator==(const LiveThread &Other) const {
    return Entry == Other.Entry && Spawn == Other.Spawn && Frame == Other.Frame &&
           Handle == Other.Handle && Joinable == Other.Joinable;
  }
};
}
// The captured handle value can be copied or overwritten independently of
// the lifetime of the thread. Key the live set by a path-local spawn number.
REGISTER_MAP_WITH_PROGRAMSTATE(KBLiveThreads, unsigned, LiveThread)
REGISTER_TRAIT_WITH_PROGRAMSTATE(KBNextThreadInstance, unsigned)

static thread_local llvm::StringSet<> SyncEvents;
static thread_local bool SyncTruncated = false;

static void emitSync(llvm::json::Object Event) {
  if (SyncTruncated) return;
  if (SyncEvents.size() >= 8192) {
    SyncTruncated = true;
    llvm::errs() << "cx-sync: {\"v\":1,\"kind\":\"incomplete\"}" << '\n';
    return;
  }
  Event["v"] = 1;
  auto Line = llvm::formatv("{0}", llvm::json::Value(std::move(Event))).str();
  if (SyncEvents.insert(Line).second) llvm::errs() << "cx-sync: " << Line << '\n';
}

static StringRef syncMode(ProgramStateRef State) {
  const auto &Options = State->getStateManager().getOwningEngine()
                          .getAnalysisManager().getAnalyzerOptions().Config;
  auto It = Options.find("cx.KB:ThreadEvidence");
  return It == Options.end() ? StringRef() : StringRef(It->second);
}

static std::string declKey(const NamedDecl *Decl) {
  if (!Decl) return {};
  const auto *Canon = cast<NamedDecl>(Decl->getCanonicalDecl());
  llvm::SmallString<128> USR;
  if (index::generateUSRForDecl(Canon, USR)) return {};
  std::string Key(USR);
  if (!Canon->isExternallyVisible()) {
    const auto &SM = Canon->getASTContext().getSourceManager();
    auto Location = SM.getExpansionLoc(Canon->getLocation());
    auto File = SM.getFilename(Location);
    if (File.empty()) return {};
    Key += "@file:" + File.str();
  }
  return Key;
}

static std::string actorKey(const StackFrame *Frame) {
  if (!Frame) return {};
  while (Frame->getParent()) Frame = Frame->getParent();
  return declKey(dyn_cast<FunctionDecl>(Frame->getDecl()));
}

static const MemRegion *lockObjectRegion(const MemRegion *Region) {
  while (const auto *Element = dyn_cast_or_null<ElementRegion>(Region)) {
    if (!Element->getIndex().isZeroConstant()) break;
    const auto *Parent = Element->getSuperRegion();
    if (const auto *Typed = dyn_cast<TypedValueRegion>(Parent))
      if (Typed->getValueType()->isArrayType()) break;
    Region = Parent;
  }
  return Region;
}

static std::string regionKey(const MemRegion *Region) {
  if (!Region) return {};
  Region = lockObjectRegion(Region);
  if (const auto *Var = dyn_cast<VarRegion>(Region)) {
    if (!Var->getDecl()->hasGlobalStorage() || Var->getDecl()->getTLSKind() != VarDecl::TLS_None)
      return {};
    return declKey(Var->getDecl());
  }
  if (const auto *Field = dyn_cast<FieldRegion>(Region)) {
    auto Key = regionKey(Field->getSuperRegion());
    auto Member = declKey(Field->getDecl());
    return Key.empty() || Member.empty() ? std::string() : Key + "/" + Member;
  }
  if (const auto *Element = dyn_cast<ElementRegion>(Region)) {
    auto Key = regionKey(Element->getSuperRegion());
    auto Index = Element->getIndex().getAs<nonloc::ConcreteInt>();
    if (Key.empty() || !Index) return {};
    return Key + "/[" + std::to_string(Index->getValue()->getExtValue()) + "]";
  }
  return {};
}

static llvm::json::Object callSite(const Expr *Call, ASTContext &AST) {
  if (!Call) return {};
  auto Loc = AST.getSourceManager().getPresumedLoc(Call->getExprLoc());
  if (!Loc.isValid()) return {};
  return llvm::json::Object{{"file",Loc.getFilename()},{"line",int64_t(Loc.getLine())},
          {"column",int64_t(Loc.getColumn())}};
}

static llvm::json::Array heldKeys(ProgramStateRef State, const MemRegion *Except = nullptr) {
  llvm::json::Array Held;
  for (const auto &[Region, Status] : State->get<LockMap>()) {
    if (!Status.isLocked() || Region == Except) continue;
    if (auto Key = regionKey(Region); !Key.empty()) Held.push_back(std::move(Key));
  }
  return Held;
}

static std::string instanceKey(unsigned Instance, const LiveThread &Thread,
                               ASTContext &AST);

static llvm::json::Array activeIds(ProgramStateRef State) {
  llvm::json::Array Active;
  for (const auto &[Instance, Thread] : State->get<KBLiveThreads>())
    if (auto Key=instanceKey(Instance,Thread,State->getStateManager().getContext()); !Key.empty())
      Active.push_back(std::move(Key));
  return Active;
}

static llvm::json::Object heldOrigins(ProgramStateRef State) {
  llvm::json::Object Sites;
  for (const auto &[Region, Origin] : State->get<KBLockOrigins>()) {
    const auto *Status = State->get<LockMap>(Region);
    if (!Status || !Status->isLocked()) continue;
    auto Key = regionKey(Region);
    if (!Key.empty()) Sites[Key] = callSite(Origin,State->getStateManager().getContext());
  }
  return Sites;
}

static void lockRequest(ProgramStateRef State, const MemRegion *Mutex,
                        const Expr *Call, const StackFrame *Frame) {
  auto Mode = syncMode(State);
  if (Mode != "locks" && Mode != "waits") return;
  auto Actor = actorKey(Frame);
  auto Request = regionKey(Mutex);
  auto Site = callSite(Call, State->getStateManager().getContext());
  if (Actor.empty() || Site.empty()) return;
  if (Request.empty()) {
    if (!Mutex || Mutex->getSymbolicBase()) emitSync(llvm::json::Object{{"kind","incomplete"}});
    return;
  }
  emitSync(llvm::json::Object{{"kind","acquire"},{"actor",std::move(Actor)},{"requested",std::move(Request)},
            {"held",heldKeys(State)},{"active",activeIds(State)},{"site",std::move(Site)}});
}

static std::string instanceKey(unsigned Instance, const LiveThread &Thread,
                               ASTContext &AST) {
  auto Loc = AST.getSourceManager().getPresumedLoc(Thread.Spawn->getExprLoc());
  auto Entry = declKey(Thread.Entry);
  if (!Loc.isValid() || Entry.empty()) return {};
  // Different paths can pick different entries at the same spawn site and
  // ordinal. Keep the entry in every reference to the instance.
  return std::to_string(Entry.size()) + ":" + Entry + actorKey(Thread.Frame) +
         "@" + Loc.getFilename() + ":" +
         std::to_string(Loc.getLine()) + ":" + std::to_string(Loc.getColumn()) +
         "#" + std::to_string(Instance);
}

static void activeThreads(ProgramStateRef State, const StackFrame *Frame) {
  auto Coordinator = actorKey(Frame);
  if (Coordinator.empty()) return;
  llvm::json::Array Threads;
  Threads.push_back(llvm::json::Object{{"entry",Coordinator},{"instance","root:"+Coordinator}});
  for (const auto &[ID, Thread] : State->get<KBLiveThreads>()) {
    auto Entry = declKey(Thread.Entry);
    auto Instance = instanceKey(ID, Thread, State->getStateManager().getContext());
    if (!Entry.empty() && !Instance.empty())
      Threads.push_back(llvm::json::Object{{"entry",std::move(Entry)},{"instance",std::move(Instance)}});
  }
  if (Threads.size() > 1)
    emitSync(llvm::json::Object{{"kind","live"},{"threads",std::move(Threads)}});
}

static const VarDecl *directVariable(const Expr *Value) {
  if (!Value) return nullptr;
  auto *Ref = dyn_cast<DeclRefExpr>(Value->IgnoreParenImpCasts());
  return Ref ? dyn_cast<VarDecl>(Ref->getDecl()) : nullptr;
}

static std::string waitPredicate(const Expr *Call, ASTContext &AST) {
  const Stmt *Current = Call;
  for (unsigned I=0; Current && I<12; ++I) {
    auto Parents = AST.getParents(*Current);
    if (Parents.size() != 1) return {};
    if (const auto *Loop = Parents[0].get<WhileStmt>()) {
      const auto *Negation = dyn_cast<UnaryOperator>(Loop->getCond()->IgnoreParenImpCasts());
      if (!Negation || Negation->getOpcode() != UO_LNot) return {};
      auto *Var = directVariable(Negation->getSubExpr());
      return Var && Var->hasGlobalStorage() ? declKey(Var) : std::string();
    }
    Current = Parents[0].get<Stmt>();
    if (Current && !isa<CompoundStmt, Expr>(Current)) return {};
  }
  return {};
}

static std::string signalPredicate(const Expr *Call, ASTContext &AST) {
  auto Parents = AST.getParents(*Call);
  if (Parents.size() != 1) return {};
  auto *Block = Parents[0].get<CompoundStmt>();
  if (!Block) return {};
  const Stmt *Previous = nullptr;
  for (const auto *Statement : Block->body()) {
    if (Statement == Call) break;
    Previous = Statement;
  }
  const auto *Assign = dyn_cast_or_null<BinaryOperator>(Previous);
  if (!Assign || Assign->getOpcode() != BO_Assign) return {};
  const auto *Var = directVariable(Assign->getLHS());
  auto Set = Assign->getRHS()->getIntegerConstantExpr(AST);
  return Var && Var->hasGlobalStorage() && Set && *Set != 0 ?
    declKey(Var) : std::string();
}

static bool threadEvent(const CallEvent &Call, CheckerContext &C) {
  auto State = C.getState();
  auto Mode = syncMode(State);
  if (Mode != "locks" && Mode != "waits") return false;
  auto *Origin = Call.getOriginExpr();
  auto Actor = actorKey(C.getStackFrame());
  if (!Origin || Actor.empty()) return false;
  if (Call.isGlobalCFunction("pthread_create") && Call.getNumArgs() == 4) {
    const auto *Storage = Call.getArgSVal(0).getAsRegion();
    const auto *Entry = Call.getArgSVal(2).getAsFunctionDecl();
    auto Return = Call.getReturnValue().getAs<DefinedSVal>();
    if (!Storage || !Entry || !Return) {
      emitSync(llvm::json::Object{{"kind","incomplete"}});
      return false;
    }
    auto [Failure, Success] = State->assume(*Return);
    if (Failure) C.addTransition(Failure);
    if (Success) {
      if (std::distance(Success->get<KBLiveThreads>().begin(), Success->get<KBLiveThreads>().end()) >= 16) {
        emitSync(llvm::json::Object{{"kind","incomplete"}});
        C.addTransition(Success);
        return true;
      }
      unsigned Instance = Success->get<KBNextThreadInstance>();
      auto Thread = LiveThread{Entry,Origin,C.getStackFrame(),Success->getSVal(Storage),
                               Success->isNull(Call.getArgSVal(1)).isConstrainedTrue()};
      Success = Success->set<KBLiveThreads>(Instance,Thread)
                       ->set<KBNextThreadInstance>(Instance + 1);
      activeThreads(Success,C.getStackFrame());
      C.addTransition(Success);
    }
    return true;
  }
  const bool Join = Call.isGlobalCFunction("pthread_join") && Call.getNumArgs() == 2;
  const bool Detach = Call.isGlobalCFunction("pthread_detach") && Call.getNumArgs() == 1;
  if (Join || Detach) {
    auto Handle = Call.getArgSVal(0);
    unsigned Instance = 0;
    const LiveThread *Thread = nullptr;
    for (const auto &[Candidate, Live] : State->get<KBLiveThreads>()) {
      if (Handle.isUnknownOrUndef() || Live.Handle.isUnknownOrUndef() ||
          !State->areEqual(Handle,Live.Handle).isConstrainedTrue()) continue;
      if (Thread) return false;
      Instance = Candidate;
      Thread = &Live;
    }
    if (!Thread) return false;
    auto Return = Call.getReturnValue().getAs<DefinedSVal>();
    if (!Return) return false;
    auto [Failure, Success] = State->assume(*Return);
    if (Success && Detach) {
      auto Detached = *Thread;
      Detached.Joinable = false;
      C.addTransition(Success->set<KBLiveThreads>(Instance,Detached));
    } else if (Success) C.addTransition(Success->remove<KBLiveThreads>(Instance));
    if (Failure) C.addTransition(Failure);
    if (Join && Mode == "waits" && Success && Thread->Joinable)
      emitSync(llvm::json::Object{{"kind","join"},{"actor",std::move(Actor)},
                {"target",instanceKey(Instance,*Thread,C.getASTContext())},{"held",heldKeys(State)},{"active",activeIds(State)},
                {"site",callSite(Origin,C.getASTContext())}});
    return true;
  }
  if (Mode != "waits") return false;
  if (Call.isGlobalCFunction("pthread_cond_wait") && Call.getNumArgs() == 2) {
    auto Return = Call.getReturnValue();
    if (State->isNull(Return).isConstrainedFalse()) return false;
    auto Cond = regionKey(Call.getArgSVal(0).getAsRegion());
    auto *Mutex = Call.getArgSVal(1).getAsRegion();
    if (Mutex) Mutex = lockObjectRegion(Mutex);
    auto Predicate = waitPredicate(Origin,C.getASTContext());
    auto MutexKey = regionKey(Mutex);
    auto *Held = Mutex ? State->get<LockMap>(Mutex) : nullptr;
    if (Held && Held->isLocked() && !Cond.empty() && !MutexKey.empty() && !Predicate.empty())
      emitSync(llvm::json::Object{{"kind","wait"},{"actor",std::move(Actor)},{"cond",std::move(Cond)},
                {"mutex",std::move(MutexKey)},{"predicate",std::move(Predicate)},
                {"held",heldKeys(State,Mutex)},{"active",activeIds(State)},{"site",callSite(Origin,C.getASTContext())}});
  } else if ((Call.isGlobalCFunction("pthread_cond_signal") ||
              Call.isGlobalCFunction("pthread_cond_broadcast")) && Call.getNumArgs() == 1) {
    auto Cond = regionKey(Call.getArgSVal(0).getAsRegion());
    auto Predicate = signalPredicate(Origin,C.getASTContext());
    emitSync(llvm::json::Object{{"kind","signal"},{"actor",std::move(Actor)},{"cond",std::move(Cond)},
              {"predicate",std::move(Predicate)},{"held",heldKeys(State)},
              {"acquisitions",heldOrigins(State)},{"site",callSite(Origin,C.getASTContext())}});
  } else if (!Call.isGlobalCFunction("pthread_mutex_lock") &&
             !Call.isGlobalCFunction("pthread_mutex_trylock") &&
             !Call.isGlobalCFunction("pthread_mutex_unlock") &&
             !Call.isGlobalCFunction("pthread_mutex_init") &&
             !Call.isGlobalCFunction("pthread_mutex_destroy") &&
             !Call.isGlobalCFunction("pthread_mutexattr_init") &&
             !Call.isGlobalCFunction("pthread_mutexattr_destroy") &&
             !Call.isGlobalCFunction("pthread_mutexattr_settype") &&
             !Call.isGlobalCFunction("pthread_cond_timedwait") &&
             !State->contains<KBLockPreservingCalls>({Origin,C.getStackFrame()}) &&
             !State->get<KBScopedLockPreservation>({Origin,C.getStackFrame()})) {
    // A conservative call can notify an otherwise uniquely identified waiter.
    emitSync(llvm::json::Object{{"kind","signal"},{"actor",std::move(Actor)},
                               {"cond",""},{"site",callSite(Origin,C.getASTContext())}});
  }
  return false;
}


static ProgramStateRef removeHeldLock(ProgramStateRef State,
                                      const MemRegion *Region) {
  auto &Factory = State->get_context<LockSet>();
  llvm::SmallVector<const MemRegion *, 8> Kept;
  for (const auto *Entry : State->get<LockSet>())
    if (Entry != Region)
      Kept.push_back(Entry);
  auto Result = Factory.getEmptyList();
  for (const auto *Entry : llvm::reverse(Kept))
    Result = Factory.add(Entry, Result);
  return State->set<LockSet>(Result)->remove<KBLockOrigins>(Region);
}

// Versioned state bridge. ProgramState traits stay in this translation unit.
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
LLVM_EXTERNAL_VISIBILITY
#endif
void clang_codexek_kb_lock_v1(
    ProgramStateRef *State, const MemRegion *Region, unsigned Operation,
    const Expr *Call, const StackFrame *Frame, void *Context,
    void (*Visit)(void *, const MemRegion *, unsigned)) {
  switch (Operation) {
  case 0:
    for (const auto &[Key, Status] : (*State)->get<LockMap>()) {
      if (Status.isLocked()) Visit(Context, Key, 1);
      else if (Status.isUnlocked()) Visit(Context, Key, 2);
    }
    return;
  case 1:
    Region = lockObjectRegion(Region);
    lockRequest(*State, Region, Call, Frame);
    *State = removeHeldLock(*State, Region);
    *State = (*State)->remove<DestroyRetVal>(Region);
    *State = (*State)->add<LockSet>(Region)->set<LockMap>(Region, LockState::getLocked());
    if (Call && syncMode(*State) == "waits")
      *State = (*State)->set<KBLockOrigins>(Region,Call);
    return;
  case 2:
    Region = lockObjectRegion(Region);
    *State = removeHeldLock(*State, Region);
    *State = (*State)->remove<DestroyRetVal>(Region)->remove<KBMutexDepths>(Region);
    *State = (*State)->set<LockMap>(Region, LockState::getUnlocked());
    return;
  case 3:
    *State = (*State)->add<KBLockPreservingCalls>({Call, Frame});
    return;
  case 4:
    *State = (*State)->remove<KBLockPreservingCalls>({Call, Frame})
                      ->remove<KBScopedLockPreservation>({Call, Frame});
    return;
  case 5: {
    Region = lockObjectRegion(Region);
    const auto *Type = (*State)->get<KBMutexTypes>(Region);
    const auto *Status = (*State)->get<LockMap>(Region);
    Visit(Context, Region, Type && *Type == KBMutexType::Normal && Status && Status->isLocked() ? 1 : 0);
    return;
  }
  case 6: {
    KBLockCallKey Key{Call, Frame};
    const auto *Prior = (*State)->get<KBScopedLockPreservation>(Key);
    Region = lockObjectRegion(Region);
    *State = (*State)->set<KBScopedLockPreservation>(
        Key, Prior ? KBPreservedRegions{Prior->first, Region}
                   : KBPreservedRegions{Region, nullptr});
    return;
  }
  }
}


bool codexekHasModeledLockPreservation(ProgramStateRef State, const CallEvent &Call) {
  if (!Call.getOriginExpr()) return false;
  KBLockCallKey Key{Call.getOriginExpr(), Call.getStackFrame()};
  return State->contains<KBLockPreservingCalls>(Key) ||
         State->get<KBScopedLockPreservation>(Key);
}

bool codexekHasHeldLock(ProgramStateRef State, SVal WaitMutex) {
  const auto *Except = WaitMutex.getAsRegion();
  if (Except) {
    Except = lockObjectRegion(Except);
    if (const auto *StateAtMutex = State->get<LockMap>(Except))
      if (StateAtMutex->isUnlocked() || StateAtMutex->isDestroyed()) return false;
  }
  for (const auto &[Region, Status] : State->get<LockMap>()) {
    if (!Status.isLocked()) continue;
    if (!Except && WaitMutex.isUnknown()) return true;
    if (!Except || Except == Region) continue;
    const auto *Base = Region->getBaseRegion();
    const auto *WaitBase = Except->getBaseRegion();
    if (Base != WaitBase && !Base->getSymbolicBase() && !WaitBase->getSymbolicBase())
      return true;
    auto SameAddress = State->areEqual(loc::MemRegionVal(Region), WaitMutex);
    if (SameAddress.isConstrainedFalse()) return true;
  }
  return false;
}

static bool nativeKBLockEnabled(ProgramStateRef State) {
  return State->getStateManager().getOwningEngine().getAnalysisManager()
             .getAnalyzerOptions().Config.lookup("cx.KB:NativeLock") == "true";
}

static ProgramStateRef invalidateLockStorage(ProgramStateRef State,
                                            const MemRegion *Changed,
                                            KBPreservedRegions Preserved = {}) {
  auto Affected = [&](const MemRegion *Region) {
    const auto *Object = lockObjectRegion(Region);
    if (Object == Preserved.first || Object == Preserved.second) return false;
    if (!Changed) return true;
    if (const auto *Space = dyn_cast<MemSpaceRegion>(Changed); Space && Preserved.first)
      return Region->getMemorySpace(State) == Space;
    return Region->getBaseRegion() == Changed->getBaseRegion() ||
           Region->getSymbolicBase() || Changed->getSymbolicBase();
  };
  for (const auto &[Region, Status] : State->get<LockMap>()) {
    if (!Affected(Region)) continue;
    State = removeHeldLock(State, Region);
    State = State->remove<LockMap>(Region)->remove<DestroyRetVal>(Region)
                ->remove<KBMutexTypes>(Region)->remove<KBMutexDepths>(Region);
  }
  for (const auto &[Region, Type] : State->get<KBMutexAttributes>()) {
    if (!Affected(Region)) continue;
    State = State->remove<KBMutexAttributes>(Region);
  }
  return State;
}

static bool containsOpaqueAssembly(const Stmt *Body, bool IncludeGNU) {
  if (!Body) return false;
  if (isa<MSAsmStmt>(Body)) return true;
  if (IncludeGNU)
    if (const auto *Assembly = dyn_cast<GCCAsmStmt>(Body))
      for (unsigned I = 0; I < Assembly->getNumClobbers(); ++I)
        if (Assembly->getClobber(I) == "memory") return true;
  for (const auto *Child : Body->children())
    if (containsOpaqueAssembly(Child, IncludeGNU)) return true;
  return false;
}

static bool suspendedLockFrame(const StackFrame *Frame) {
  for (; Frame; Frame = Frame->getParent())
    if (containsOpaqueAssembly(Frame->getDecl()->getBody(), false)) return true;
  return false;
}

void PthreadLockChecker::checkBeginFunction(CheckerContext &C) const {
  if (nativeKBLockEnabled(C.getState()) && suspendedLockFrame(C.getStackFrame()))
    C.addTransition(invalidateLockStorage(C.getState(), nullptr));
}

void PthreadLockChecker::checkBind(SVal Location, SVal, const Stmt *, bool,
                                   CheckerContext &C) const {
  if (nativeKBLockEnabled(C.getState()))
    C.addTransition(invalidateLockStorage(C.getState(), Location.getAsRegion()));
}

void PthreadLockChecker::checkPreStmt(const GCCAsmStmt *Assembly,
                                      CheckerContext &C) const {
  if (!nativeKBLockEnabled(C.getState())) return;
  for (unsigned I = 0; I < Assembly->getNumClobbers(); ++I)
    if (Assembly->getClobber(I) == "memory") {
      C.addTransition(invalidateLockStorage(C.getState(), nullptr));
      return;
    }
}

static std::optional<KBMutexType> decodeMutexType(const CallEvent &Call,
                                                  CheckerContext &C) {
  auto Number = Call.getArgSVal(1).getAs<nonloc::ConcreteInt>();
  if (!Number) return std::nullopt;
  auto &PP = C.getBugReporter().getPreprocessor();
  for (const auto &[Name, Type] : {
         std::pair<StringRef, KBMutexType>{"PTHREAD_MUTEX_RECURSIVE", KBMutexType::Recursive},
         {"PTHREAD_MUTEX_ERRORCHECK", KBMutexType::ErrorCheck},
         {"PTHREAD_MUTEX_NORMAL", KBMutexType::Normal}}) {
    auto MacroValue = tryExpandAsInteger(Name, PP);
    if (MacroValue && Number->getValue()->getExtValue() == *MacroValue) return Type;
  }
  return std::nullopt;
}

void PthreadLockChecker::checkPostCall(const CallEvent &Call,
                                       CheckerContext &C) const {
  // FIXME: Try to handle cases when the implementation was inlined rather
  // than just giving up.
  if (C.wasInlined ||
      (nativeKBLockEnabled(C.getState()) && suspendedLockFrame(C.getStackFrame())))
    return;
  if (threadEvent(Call,C))
    return;

  if (nativeKBLockEnabled(C.getState()) &&
      Call.isGlobalCFunction("pthread_mutexattr_settype") && Call.getNumArgs() == 2) {
    auto *Attr = Call.getArgSVal(0).getAsRegion();
    auto Return = Call.getReturnValue().getAs<DefinedSVal>();
    if (!Attr || !Return) return;
    Attr = lockObjectRegion(Attr);
    auto [Failure, Success] = C.getState()->assume(*Return);
    if (Failure) C.addTransition(Failure);
    if (Success) {
      auto Type = decodeMutexType(Call, C);
      Success = Type ? Success->set<KBMutexAttributes>(Attr, *Type)
                     : Success->remove<KBMutexAttributes>(Attr);
      C.addTransition(Success);
    }
    return;
  }

  if (const FnCheck *Callback = PThreadCallbacks.lookup(Call))
    (this->**Callback)(Call, C, CK_PthreadLockChecker);
  else if (const FnCheck *Callback = FuchsiaCallbacks.lookup(Call))
    (this->**Callback)(Call, C, CK_FuchsiaLockChecker);
  else if (const FnCheck *Callback = C11Callbacks.lookup(Call))
    (this->**Callback)(Call, C, CK_C11LockChecker);
}

// When a lock is destroyed, in some semantics(like PthreadSemantics) we are not
// sure if the destroy call has succeeded or failed, and the lock enters one of
// the 'possibly destroyed' state. There is a short time frame for the
// programmer to check the return value to see if the lock was successfully
// destroyed. Before we model the next operation over that lock, we call this
// function to see if the return value was checked by now and set the lock state
// - either to destroyed state or back to its previous state.

// In PthreadSemantics, pthread_mutex_destroy() returns zero if the lock is
// successfully destroyed and it returns a non-zero value otherwise.
ProgramStateRef PthreadLockChecker::resolvePossiblyDestroyedMutex(
    ProgramStateRef state, const MemRegion *lockR, const SymbolRef *sym) const {
  const LockState *lstate = state->get<LockMap>(lockR);
  // Existence in DestroyRetVal ensures existence in LockMap.
  // Existence in Destroyed also ensures that the lock state for lockR is either
  // UntouchedAndPossiblyDestroyed or UnlockedAndPossiblyDestroyed.
  assert(lstate);
  assert(lstate->isUntouchedAndPossiblyDestroyed() ||
         lstate->isUnlockedAndPossiblyDestroyed());

  ConstraintManager &CMgr = state->getConstraintManager();
  ConditionTruthVal retZero = CMgr.isNull(state, *sym);
  if (retZero.isConstrainedFalse()) {
    if (lstate->isUntouchedAndPossiblyDestroyed())
      state = state->remove<LockMap>(lockR);
    else if (lstate->isUnlockedAndPossiblyDestroyed())
      state = state->set<LockMap>(lockR, LockState::getUnlocked());
  } else
    state = state->set<LockMap>(lockR, LockState::getDestroyed());

  // Removing the map entry (lockR, sym) from DestroyRetVal as the lock state is
  // now resolved.
  state = state->remove<DestroyRetVal>(lockR);
  return state;
}

void PthreadLockChecker::printState(raw_ostream &Out, ProgramStateRef State,
                                    const char *NL, const char *Sep) const {
  LockMapTy LM = State->get<LockMap>();
  if (!LM.isEmpty()) {
    Out << Sep << "Mutex states:" << NL;
    for (auto I : LM) {
      I.first->dumpToStream(Out);
      if (I.second.isLocked())
        Out << ": locked";
      else if (I.second.isUnlocked())
        Out << ": unlocked";
      else if (I.second.isDestroyed())
        Out << ": destroyed";
      else if (I.second.isUntouchedAndPossiblyDestroyed())
        Out << ": not tracked, possibly destroyed";
      else if (I.second.isUnlockedAndPossiblyDestroyed())
        Out << ": unlocked, possibly destroyed";
      Out << NL;
    }
  }

  LockSetTy LS = State->get<LockSet>();
  if (!LS.isEmpty()) {
    Out << Sep << "Mutex lock order:" << NL;
    for (auto I : LS) {
      I->dumpToStream(Out);
      Out << NL;
    }
  }

  DestroyRetValTy DRV = State->get<DestroyRetVal>();
  if (!DRV.isEmpty()) {
    Out << Sep << "Mutexes in unresolved possibly destroyed state:" << NL;
    for (auto I : DRV) {
      I.first->dumpToStream(Out);
      Out << ": ";
      I.second->dumpToStream(Out);
      Out << NL;
    }
  }
}

void PthreadLockChecker::AcquirePthreadLock(const CallEvent &Call,
                                            CheckerContext &C,
                                            CheckerKind CheckKind) const {
  AcquireLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), false,
                 PthreadSemantics, CheckKind);
}

void PthreadLockChecker::AcquireXNULock(const CallEvent &Call,
                                        CheckerContext &C,
                                        CheckerKind CheckKind) const {
  AcquireLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), false,
                 XNUSemantics, CheckKind);
}

void PthreadLockChecker::TryPthreadLock(const CallEvent &Call,
                                        CheckerContext &C,
                                        CheckerKind CheckKind) const {
  AcquireLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), true,
                 PthreadSemantics, CheckKind);
}

void PthreadLockChecker::TryXNULock(const CallEvent &Call, CheckerContext &C,
                                    CheckerKind CheckKind) const {
  AcquireLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), true,
                 PthreadSemantics, CheckKind);
}

void PthreadLockChecker::TryFuchsiaLock(const CallEvent &Call,
                                        CheckerContext &C,
                                        CheckerKind CheckKind) const {
  AcquireLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), true,
                 PthreadSemantics, CheckKind);
}

void PthreadLockChecker::TryC11Lock(const CallEvent &Call, CheckerContext &C,
                                    CheckerKind CheckKind) const {
  AcquireLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), true,
                 PthreadSemantics, CheckKind);
}

void PthreadLockChecker::AcquireLockAux(const CallEvent &Call,
                                        CheckerContext &C, const Expr *MtxExpr,
                                        SVal MtxVal, bool IsTryLock,
                                        enum LockingSemantics Semantics,
                                        CheckerKind CheckKind) const {
  if (!ChecksEnabled[CheckKind])
    return;

  const MemRegion *lockR = MtxVal.getAsRegion();
  if (!lockR)
    return;
  if (nativeKBLockEnabled(C.getState()))
    lockR = lockObjectRegion(lockR);

  ProgramStateRef state = C.getState();
  const SymbolRef *sym = state->get<DestroyRetVal>(lockR);
  if (sym)
    state = resolvePossiblyDestroyedMutex(state, lockR, sym);

  if (const LockState *LState = state->get<LockMap>(lockR)) {
    if (LState->isLocked()) {
      if (nativeKBLockEnabled(state) &&
          (Call.isGlobalCFunction("pthread_mutex_lock") ||
           Call.isGlobalCFunction("pthread_mutex_trylock"))) {
        const auto *Type = state->get<KBMutexTypes>(lockR);
        if (IsTryLock && (!Type || *Type == KBMutexType::Normal)) {
          if (Type)
            if (auto Return = Call.getReturnValue().getAs<DefinedSVal>())
              if (auto Failure = state->assume(*Return,true)) C.addTransition(Failure);
          return;
        }
        if (Type && *Type == KBMutexType::Normal && !IsTryLock &&
            C.getAnalysisManager().getAnalyzerOptions().Config.lookup(
                "cx.KB:ThreadSelfDeadlock") == "true") {
          reportBug(C, BT_selfdeadlock, MtxExpr, lockR, CheckKind,
                    "Thread self-deadlock: blocking acquisition of its own non-recursive mutex");
          return;
        }
        if (Type && (*Type == KBMutexType::Recursive || *Type == KBMutexType::ErrorCheck)) {
          auto Return = Call.getReturnValue().getAs<DefinedSVal>();
          if (!Return) return;
          auto [Failure, Success] = state->assume(*Return);
          if (Failure) C.addTransition(Failure);
          if (Success && *Type == KBMutexType::Recursive) {
            auto *Depth = Success->get<KBMutexDepths>(lockR);
            C.addTransition(Success->set<KBMutexDepths>(lockR, Depth ? *Depth + 1 : 2));
          } else if (!Failure) {
            // A concrete result conflicting with the library contract is not modeled.
            C.addTransition(state);
          }
          return;
        }
      }
      reportBug(C, BT_doublelock, MtxExpr, lockR, CheckKind,
                "This lock has already been acquired");
      return;
    } else if (LState->isDestroyed()) {
      reportBug(C, BT_destroylock, MtxExpr, lockR, CheckKind,
                "This lock has already been destroyed");
      return;
    }
  }

  ProgramStateRef lockSucc = state;
  if (IsTryLock) {
    // Bifurcate the state, and allow a mode where the lock acquisition fails.
    SVal RetVal = Call.getReturnValue();
    if (auto DefinedRetVal = RetVal.getAs<DefinedSVal>()) {
      ProgramStateRef lockFail;
      switch (Semantics) {
      case PthreadSemantics:
        std::tie(lockFail, lockSucc) = state->assume(*DefinedRetVal);
        break;
      case XNUSemantics:
        std::tie(lockSucc, lockFail) = state->assume(*DefinedRetVal);
        break;
      default:
        llvm_unreachable("Unknown tryLock locking semantics");
      }
      assert(lockFail && lockSucc);
      C.addTransition(lockFail);
    }
    // We might want to handle the case when the mutex lock function was inlined
    // and returned an Unknown or Undefined value.
  } else if (Semantics == PthreadSemantics) {
    // Assume that the return value was 0.
    SVal RetVal = Call.getReturnValue();
    if (auto DefinedRetVal = RetVal.getAs<DefinedSVal>()) {
      // FIXME: If the lock function was inlined and returned true,
      // we need to behave sanely - at least generate sink.
      if (nativeKBLockEnabled(state)) {
        auto [Failed, Succeeded] = state->assume(*DefinedRetVal);
        if (Failed)
          C.addTransition(Failed);
        if (!Succeeded)
          return;
        lockSucc = Succeeded;
      } else {
        lockSucc = state->assume(*DefinedRetVal, false);
        assert(lockSucc);
      }
    }
    // We might want to handle the case when the mutex lock function was inlined
    // and returned an Unknown or Undefined value.
  } else {
    // XNU locking semantics return void on non-try locks
    assert((Semantics == XNUSemantics) && "Unknown locking semantics");
    lockSucc = state;
  }

  if (!IsTryLock && Call.isGlobalCFunction("pthread_mutex_lock"))
    lockRequest(state,lockR,Call.getOriginExpr(),C.getStackFrame());

  // Record that the lock was acquired.
  lockSucc = lockSucc->add<LockSet>(lockR);
  lockSucc = lockSucc->set<LockMap>(lockR, LockState::getLocked());
  if (syncMode(lockSucc) == "waits" && Call.getOriginExpr())
    lockSucc = lockSucc->set<KBLockOrigins>(lockR,Call.getOriginExpr());
  C.addTransition(lockSucc,
                  createMutexNote(C, lockR, "Locking ", "Mutex acquired here"));
}

void PthreadLockChecker::ReleaseAnyLock(const CallEvent &Call,
                                        CheckerContext &C,
                                        CheckerKind CheckKind) const {
  ReleaseLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), CheckKind);
}

void PthreadLockChecker::ReleaseLockAux(const CallEvent &Call,
                                        CheckerContext &C, const Expr *MtxExpr,
                                        SVal MtxVal,
                                        CheckerKind CheckKind) const {
  if (!ChecksEnabled[CheckKind])
    return;

  const MemRegion *lockR = MtxVal.getAsRegion();
  if (!lockR)
    return;
  if (nativeKBLockEnabled(C.getState()))
    lockR = lockObjectRegion(lockR);

  ProgramStateRef state = C.getState();
  const SymbolRef *sym = state->get<DestroyRetVal>(lockR);
  if (sym)
    state = resolvePossiblyDestroyedMutex(state, lockR, sym);

  if (const LockState *LState = state->get<LockMap>(lockR)) {
    if (LState->isUnlocked()) {
      reportBug(C, BT_doubleunlock, MtxExpr, lockR, CheckKind,
                "This lock has already been unlocked");
      return;
    } else if (LState->isDestroyed()) {
      reportBug(C, BT_destroylock, MtxExpr, lockR, CheckKind,
                "This lock has already been destroyed");
      return;
    }
  }

  if (nativeKBLockEnabled(state) && Call.isGlobalCFunction("pthread_mutex_unlock")) {
    if (auto Ret = Call.getReturnValue().getAs<DefinedSVal>()) {
      auto [Failed, Succeeded] = state->assume(*Ret);
      if (Failed) C.addTransition(Failed);
      if (!Succeeded) return;
      state = Succeeded;
    } else
      return;
  }

  if (nativeKBLockEnabled(state) && Call.isGlobalCFunction("pthread_mutex_unlock"))
    if (auto *Depth = state->get<KBMutexDepths>(lockR)) {
      C.addTransition(*Depth == 2 ? state->remove<KBMutexDepths>(lockR)
                                 : state->set<KBMutexDepths>(lockR, *Depth - 1));
      return;
    }

  LockSetTy LS = state->get<LockSet>();

  if (!LS.isEmpty()) {
    if (WarnOnLockOrderReversal && LS.getHead() != lockR) {
      reportBug(C, BT_lor, MtxExpr, lockR, CheckKind,
                "This was not the most recently acquired lock. Possible lock "
                "order reversal",
                LS.getHead());
      return;
    }

    auto &Factory = state->get_context<LockSet>();
    llvm::ImmutableList<const MemRegion *> NewLS = Factory.getEmptyList();
    for (const MemRegion *LockReg :
         llvm::make_filter_range(LS, llvm::not_equal_to(lockR))) {
      NewLS = Factory.add(LockReg, NewLS);
    }
    state = state->set<LockSet>(NewLS);
  }

  state = state->set<LockMap>(lockR, LockState::getUnlocked())->remove<KBLockOrigins>(lockR);
  C.addTransition(
      state, createMutexNote(C, lockR, "Unlocking ", "Mutex released here"));
}

void PthreadLockChecker::DestroyPthreadLock(const CallEvent &Call,
                                            CheckerContext &C,
                                            CheckerKind CheckKind) const {
  DestroyLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0),
                 PthreadSemantics, CheckKind);
}

void PthreadLockChecker::DestroyXNULock(const CallEvent &Call,
                                        CheckerContext &C,
                                        CheckerKind CheckKind) const {
  DestroyLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), XNUSemantics,
                 CheckKind);
}

void PthreadLockChecker::DestroyLockAux(const CallEvent &Call,
                                        CheckerContext &C, const Expr *MtxExpr,
                                        SVal MtxVal,
                                        enum LockingSemantics Semantics,
                                        CheckerKind CheckKind) const {
  if (!ChecksEnabled[CheckKind])
    return;

  const MemRegion *LockR = MtxVal.getAsRegion();
  if (!LockR)
    return;

  ProgramStateRef State = C.getState();

  const SymbolRef *sym = State->get<DestroyRetVal>(LockR);
  if (sym)
    State = resolvePossiblyDestroyedMutex(State, LockR, sym);

  const LockState *LState = State->get<LockMap>(LockR);
  // Checking the return value of the destroy method only in the case of
  // PthreadSemantics
  if (Semantics == PthreadSemantics) {
    if (!LState || LState->isUnlocked()) {
      SymbolRef sym = Call.getReturnValue().getAsSymbol();
      if (!sym) {
        State = State->remove<LockMap>(LockR);
        C.addTransition(State, createMutexNote(C, LockR, "Destroying ",
                                               "Mutex destroyed here"));
        return;
      }
      State = State->set<DestroyRetVal>(LockR, sym);
      if (LState && LState->isUnlocked())
        State = State->set<LockMap>(
            LockR, LockState::getUnlockedAndPossiblyDestroyed());
      else
        State = State->set<LockMap>(
            LockR, LockState::getUntouchedAndPossiblyDestroyed());
      C.addTransition(State, createMutexNote(C, LockR, "Destroying ",
                                             "Mutex destroyed here"));
      return;
    }
  } else {
    if (!LState || LState->isUnlocked()) {
      State = State->set<LockMap>(LockR, LockState::getDestroyed());
      C.addTransition(State, createMutexNote(C, LockR, "Destroying ",
                                             "Mutex destroyed here"));
      return;
    }
  }

  StringRef Message = LState->isLocked()
                          ? "This lock is still locked"
                          : "This lock has already been destroyed";

  reportBug(C, BT_destroylock, MtxExpr, LockR, CheckKind, Message);
}

void PthreadLockChecker::InitAnyLock(const CallEvent &Call, CheckerContext &C,
                                     CheckerKind CheckKind) const {
  InitLockAux(Call, C, Call.getArgExpr(0), Call.getArgSVal(0), CheckKind);
}

void PthreadLockChecker::InitLockAux(const CallEvent &Call, CheckerContext &C,
                                     const Expr *MtxExpr, SVal MtxVal,
                                     CheckerKind CheckKind) const {
  if (!ChecksEnabled[CheckKind])
    return;

  const MemRegion *LockR = MtxVal.getAsRegion();
  if (!LockR)
    return;

  ProgramStateRef State = C.getState();

  const SymbolRef *sym = State->get<DestroyRetVal>(LockR);
  if (sym)
    State = resolvePossiblyDestroyedMutex(State, LockR, sym);

  if (nativeKBLockEnabled(State)) LockR = lockObjectRegion(LockR);
  const struct LockState *LState = State->get<LockMap>(LockR);
  if (!LState || LState->isDestroyed()) {
    if (nativeKBLockEnabled(State) && Call.isGlobalCFunction("pthread_mutex_init")) {
      auto Return = Call.getReturnValue().getAs<DefinedSVal>();
      if (!Return) return;
      auto [Failure, Success] = State->assume(*Return);
      if (Failure) C.addTransition(Failure);
      if (!Success) return;
      State = Success->remove<KBMutexTypes>(LockR)->remove<KBMutexDepths>(LockR);
      if (const auto *Attr = Call.getArgSVal(1).getAsRegion())
        if (const auto *Type = State->get<KBMutexAttributes>(lockObjectRegion(Attr)))
          State = State->set<KBMutexTypes>(LockR, *Type);
    }
    State = State->set<LockMap>(LockR, LockState::getUnlocked());
    C.addTransition(State, createMutexNote(C, LockR, "Initializing ",
                                           "Mutex initialized here"));
    return;
  }

  StringRef Message = LState->isLocked()
                          ? "This lock is still being held"
                          : "This lock has already been initialized";

  reportBug(C, BT_initlock, MtxExpr, LockR, CheckKind, Message);
}

void PthreadLockChecker::reportBug(CheckerContext &C,
                                   std::unique_ptr<BugType> BT[],
                                   const Expr *MtxExpr,
                                   const MemRegion *MtxRegion,
                                   CheckerKind CheckKind, StringRef Desc,
                                   const MemRegion *ExtraInteresting) const {
  ExplodedNode *N = C.generateErrorNode();
  if (!N)
    return;
  initBugType(CheckKind);
  auto Report =
      std::make_unique<PathSensitiveBugReport>(*BT[CheckKind], Desc, N);
  Report->addRange(MtxExpr->getSourceRange());
  if (MtxRegion)
    Report->markInteresting(MtxRegion);
  if (ExtraInteresting)
    Report->markInteresting(ExtraInteresting);
  C.emitReport(std::move(Report));
}

void PthreadLockChecker::checkDeadSymbols(SymbolReaper &SymReaper,
                                          CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  for (auto I : State->get<DestroyRetVal>()) {
    // Once the return value symbol dies, no more checks can be performed
    // against it. See if the return value was checked before this point.
    // This would remove the symbol from the map as well.
    if (SymReaper.isDead(I.second))
      State = resolvePossiblyDestroyedMutex(State, I.first, &I.second);
  }

  for (auto I : State->get<LockMap>()) {
    // Stop tracking dead mutex regions as well.
    if (!SymReaper.isLiveRegion(I.first)) {
      State = State->remove<LockMap>(I.first);
      State = State->remove<DestroyRetVal>(I.first)->remove<KBLockOrigins>(I.first)
                  ->remove<KBMutexTypes>(I.first)->remove<KBMutexDepths>(I.first);
    }
  }
  for (const auto &[Region, Type] : State->get<KBMutexAttributes>())
    if (!SymReaper.isLiveRegion(Region)) State = State->remove<KBMutexAttributes>(Region);

  // TODO: We probably need to clean up the lock stack as well.
  // It is tricky though: even if the mutex cannot be unlocked anymore,
  // it can still participate in lock order reversal resolution.

  C.addTransition(State);
}

ProgramStateRef PthreadLockChecker::checkRegionChanges(
    ProgramStateRef State, const InvalidatedSymbols *Symbols,
    ArrayRef<const MemRegion *> ExplicitRegions,
    ArrayRef<const MemRegion *> Regions, const StackFrame *SF,
    const CallEvent *Call) const {

  const bool NativeKB = nativeKBLockEnabled(State);
  if (NativeKB && Call && Call->getDecl() && containsOpaqueAssembly(Call->getDecl()->getBody(), true))
    return invalidateLockStorage(State, nullptr);
  KBPreservedRegions Preserved;
  if (Call && Call->getOriginExpr()) {
    KBLockCallKey Key{Call->getOriginExpr(), SF};
    if (State->contains<KBLockPreservingCalls>(Key)) return State;
    if (const auto *Scoped = State->get<KBScopedLockPreservation>(Key))
      Preserved = *Scoped;
  }

  bool IsLibraryFunction = false;
  if (Call && Call->isGlobalCFunction() && !Preserved.first) {
    if (NativeKB && (Call->isGlobalCFunction("pthread_cond_wait") ||
                     Call->isGlobalCFunction("pthread_cond_timedwait") ||
                     Call->isGlobalCFunction("pthread_mutexattr_settype") ||
                     Call->isGlobalCFunction("pthread_create") ||
                     Call->isGlobalCFunction("pthread_join") ||
                     Call->isGlobalCFunction("pthread_detach") ||
                     Call->isGlobalCFunction("pthread_cond_signal") ||
                     Call->isGlobalCFunction("pthread_cond_broadcast")))
      return State;
    // Avoid invalidating mutex state when a known supported function is called.
    if (PThreadCallbacks.lookup(*Call) || FuchsiaCallbacks.lookup(*Call) ||
        C11Callbacks.lookup(*Call))
      return State;

    if (Call->isInSystemHeader())
      IsLibraryFunction = true;
  }

  for (auto R : Regions) {
    // We assume that system library function wouldn't touch the mutex unless
    // it takes the mutex explicitly as an argument.
    // FIXME: This is a bit quadratic.
    if (IsLibraryFunction && !llvm::is_contained(ExplicitRegions, R))
      continue;

    if (NativeKB)
      State = invalidateLockStorage(State, R, Preserved);
    else {
      State = State->remove<LockMap>(R);
      State = State->remove<DestroyRetVal>(R);
    }

    // TODO: We need to invalidate the lock stack as well. This is tricky
    // to implement correctly and efficiently though, because the effects
    // of mutex escapes on lock order may be fairly varied.
  }
  if (NativeKB && Preserved.first) {
    if (Regions.empty() && ExplicitRegions.empty())
      return invalidateLockStorage(State, nullptr, Preserved);
    for (auto R : ExplicitRegions)
      State = invalidateLockStorage(State, R, Preserved);
  }

  return State;
}

void ento::registerPthreadLockBase(CheckerManager &mgr) {
  mgr.registerChecker<PthreadLockChecker>();
}

bool ento::shouldRegisterPthreadLockBase(const CheckerManager &mgr) { return true; }

#define REGISTER_CHECKER(name)                                                 \
  void ento::register##name(CheckerManager &mgr) {                             \
    PthreadLockChecker *checker = mgr.getChecker<PthreadLockChecker>();        \
    checker->ChecksEnabled[PthreadLockChecker::CK_##name] = true;              \
    checker->CheckNames[PthreadLockChecker::CK_##name] =                       \
        mgr.getCurrentCheckerName();                                           \
  }                                                                            \
                                                                               \
  bool ento::shouldRegister##name(const CheckerManager &mgr) { return true; }

REGISTER_CHECKER(FuchsiaLockChecker)
REGISTER_CHECKER(C11LockChecker)

#undef REGISTER_CHECKER

void ento::registerPthreadLockChecker(CheckerManager &Mgr) {
  PthreadLockChecker *Checker = Mgr.getChecker<PthreadLockChecker>();
  Checker->ChecksEnabled[PthreadLockChecker::CK_PthreadLockChecker] = true;
  Checker->CheckNames[PthreadLockChecker::CK_PthreadLockChecker] =
      Mgr.getCurrentCheckerName();
  Checker->WarnOnLockOrderReversal =
      Mgr.getAnalyzerOptions().getCheckerBooleanOption(
          Mgr.getCurrentCheckerName(), "WarnOnLockOrderReversal");
  auto Mode = Mgr.getAnalyzerOptions().Config.lookup("cx.KB:ThreadEvidence");
  if (Mode == "locks" || Mode == "waits") {
    SyncEvents.clear();
    SyncTruncated = false;
    emitSync(llvm::json::Object{{"kind","begin"},{"mode",Mode}});
  }
}

bool ento::shouldRegisterPthreadLockChecker(const CheckerManager &) {
  return true;
}

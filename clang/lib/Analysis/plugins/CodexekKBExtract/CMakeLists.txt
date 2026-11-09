# codexek KB extractor plugin (KB-6). Built WITHOUT linking LLVM/clang
# libraries: the plugin resolves all clang symbols at load time from the
# host clang process (same deployment model as the product CxKB module).
# Static linkage would duplicate LLVM global state ("option registered more
# than once" at -load time).
if(CLANG_PLUGIN_SUPPORT AND LLVM_ENABLE_PLUGINS)
  add_library(CodexekKBExtract MODULE CodexekKBExtract.cpp)
  # The clang CMake tree propagates directory-scope link libraries (LLVM
  # Support etc.) onto every target; a plugin must NOT statically carry them
  # (duplicate LLVM global state -> "option registered more than once" at
  # -load time). Explicitly empty LINK_LIBRARIES breaks that inheritance;
  # all clang symbols resolve at load time from the host clang process.
  set_target_properties(CodexekKBExtract PROPERTIES LINK_LIBRARIES "")
  target_include_directories(CodexekKBExtract PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    "${CMAKE_BINARY_DIR}/tools/clang/include")
  if(APPLE)
    target_link_options(CodexekKBExtract PRIVATE "-Wl,-undefined,dynamic_lookup")
  elseif(UNIX)
    target_link_options(CodexekKBExtract PRIVATE "-Wl,-z,undefs")
  endif()
endif()

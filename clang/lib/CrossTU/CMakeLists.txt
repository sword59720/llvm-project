set(LLVM_LINK_COMPONENTS
  Support
  TargetParser
  )

add_clang_library(clangCrossTU
  CrossTranslationUnit.cpp

  LINK_LIBS
  clangAST
  clangBasic
  clangDriver
  clangFrontend
  clangUnifiedSymbolResolution
  )

# codeXek's on-disk CTU index. The upstream text index remains supported.
option(CLANG_ENABLE_CTU_SQLITE "Enable the codeXek read-only SQLite CTU index" OFF)
if(CLANG_ENABLE_CTU_SQLITE)
  find_package(SQLite3 REQUIRED)
  set_property(TARGET SQLite::SQLite3 PROPERTY IMPORTED_GLOBAL TRUE)
  target_compile_definitions(clangCrossTU PRIVATE CLANG_ENABLE_CTU_SQLITE=1)
  # add_clang_library compiles sources through a separate object target;
  # definitions added after the call only reach it explicitly.
  if(TARGET obj.clangCrossTU)
    target_compile_definitions(obj.clangCrossTU PRIVATE CLANG_ENABLE_CTU_SQLITE=1)
  endif()
  target_link_libraries(clangCrossTU PRIVATE SQLite::SQLite3)
endif()

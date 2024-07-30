set(LLVM_LINK_COMPONENTS support)

add_clang_executable(move-adder
  MoveAdder.cpp
  )
target_link_libraries(move-adder
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangSerialization
  clangTooling
  )

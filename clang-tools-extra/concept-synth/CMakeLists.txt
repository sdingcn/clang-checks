set(LLVM_LINK_COMPONENTS support)

add_clang_executable(concept-synth
  ConceptSynth.cpp
  )
target_link_libraries(concept-synth
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangSerialization
  clangTooling
  )

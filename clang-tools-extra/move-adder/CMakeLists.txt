set(LLVM_LINK_COMPONENTS support)
set(JSON_BuildTests OFF CACHE INTERNAL "")

add_subdirectory(json)

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
  nlohmann_json::nlohmann_json
  )

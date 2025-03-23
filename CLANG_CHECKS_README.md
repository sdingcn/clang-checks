# clang-checks

This repository contains the move-adder based on clang.

```
mkdir build
cd build
cmake -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles" ../llvm
make -j8 <target>
```

## move-adder

The move-adder checks C++ code for cases
where objects are unnecessarily copied.
In those cases it's better to use `std::move`
to trigger move constructors or move assignment operators.

### source

```
clang-tools-extra/move-adder/MoveAdder.cpp
```

### run

#### Run the check on a single C++ source file

```
build/bin/move-adder <cpp-file> --
```

Note: You may need to include library paths after the `--`.

#### Run the check on a C++ project

First generate the `compile_commands.json` file.
For example, if the project is based on CMake,
you can run `cmake` using the option `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
to generate the corresponding `compile_commands.json` file.

Then use the following command to run the check
on the entire C++ project.
```
move-analysis-scripts/run.sh <path to compile-commands.json> <test-command> <path-to-build-dir> <build-command>
```

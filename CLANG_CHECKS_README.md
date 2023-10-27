# clang-checks

This repository contains two C++ checks based on clang.
The following commands build both checks.

```
mkdir build
cd build
cmake -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles" ../llvm
make -j8
```

## performance-missing-move

The performance-missing-move check is based on clang-tidy
(https://clang.llvm.org/extra/clang-tidy/),
which checks C++ code for cases where objects are copied but
those copies are unnecessary. In those cases
it's better to use `std::move` to trigger the move constructors or
move assignment operators.

```
// before
vector<int> v = v0;
for (int x : v) cout << x << endl;
return;

// after
// Because v0 is never used after being copied into v, it's better to move it into v.
vector<int> v = std::move(v0);
for (int x : v) cout << x << endl;
return;
```

### source

```
clang-tools-extra/clang-tidy/performance/MissingMovesCheck.h
clang-tools-extra/clang-tidy/performance/MissingMovesCheck.cpp
```

### run

#### Run the check on a single C++ source file

```
build/bin/clang-tidy test.cc -checks="-*,performance-missing-moves" --
```

Note: You may need to include library paths after the `--`.

#### Run the check on a C++ project

First generate the `compile_commands.json` file.
For example, if the project is based on CMake,
you can run `cmake` using the option `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
to generate the corresponding `compile_commands.json` file.

Then use the following command to run clang-tidy
on the entire C++ project.
```
clang-tools-extra/clang-tidy/tool/run-clang-tidy.py -clang-tidy-binary build/bin/clang-tidy -p <path-to-the-build-folder-of-the-C++-project> -checks="-*,performance-missing-moves" -quiet
```

Or use the following command to run clang-tidy and apply fixings (adding `std::move`) to
every found place.
```
clang-tools-extra/clang-tidy/tool/run-clang-tidy.py -fix -clang-tidy-binary build/bin/clang-tidy -clang-apply-replacements-binary build/bin/clang-apply-replacements -p <path-to-the-build-folder-of-the-C++-project> -checks="-*,performance-missing-moves" -quiet
```

Note: The current checker is incomplete in the sense that there are some corner cases
where blindly applying all fixings might introduce errors (it is related to function overloading
resolution), so it is recommended to manually
check each replace to make sure it is correct.

## concept-synth

The concept-synth check is based on clang's RecursiveASTVisitor,
which scans all function templates and synthesize concepts (C++20) for them
based on (1) the template body and (2) the instantiations.

### source

```
clang-tools-extra/concept-synth/ConceptSynth.cpp
```

### run

#### Run the check on a single C++ source file

```
build/bin/concept-synth test.cc --
```

Note: You may need to include library paths after the `--`.

#### Run the check on a C++ project

TODO
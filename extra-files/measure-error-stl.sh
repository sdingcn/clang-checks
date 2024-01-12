clang++ -std=c++20 -E --no-line-commands stdtest.cc > prep.cc
../build/bin/concept-synth prep.cc -- -std=c++20 > out.txt
python3 measure-error.py prep.cc out.txt std

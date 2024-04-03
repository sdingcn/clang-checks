import sys
import os.path
from typing import List, Tuple
from utils import *

if __name__ == '__main__':
    compiler = "clang++"
    src_path = "std-src.cc"
    prep_path = os.path.join("std-doc", "prep.cc")
    result_path = os.path.join("std-doc", "result.txt")

    prep = preprocess(compiler, src_path, [])
    with open(prep_path, "w") as f1:
        f1.write(prep)
    result = synth(prep_path)
    with open(result_path, "w") as f2:
        f2.write(result)

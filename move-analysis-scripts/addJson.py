from json import loads, dumps
import sys

commands = [
    "/usr/lib/gcc/x86_64-linux-gnu/11/../../../../include/c++/11",
    "/usr/lib/gcc/x86_64-linux-gnu/11/../../../../include/x86_64-linux-gnu/c++/11",
    "/usr/lib/gcc/x86_64-linux-gnu/11/../../../../include/c++/11/backward",
    "/usr/local/include",
    "/data/qirun/soft/llvm/17/bin/lib/clang/17/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include"
]


if (__name__ == "__main__"):
    if (len(sys.argv) != 2):
        print("usage: python3 addJson <path-to-compile-comands.json>")
        sys.exit(1)
    path = sys.argv[1]
    with open(path) as f:
        jsonInfo = loads(f.read())
    for j in jsonInfo:
        for c in commands:
            j["command"] += " -I" + c
    
    with open(path, 'w') as f:
        f.write(dumps(jsonInfo))

from json import loads, dumps
import sys

commands = [
    "/opt/homebrew/opt/llvm@17/bin/../include/c++/v1",
    "/opt/homebrew/Cellar/llvm@17/17.0.6/lib/clang/17/include",
    "/Library/Developer/CommandLineTools/SDKs/MacOSX14.sdk/usr/include",
    "/Library/Developer/CommandLineTools/SDKs/MacOSX14.sdk/System/Library/Frameworks"
    "/usr/local/include",
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

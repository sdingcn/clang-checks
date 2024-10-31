from json import loads, dumps
import sys

commands = [
    "/usr/local/include",
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1",
    "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/15.0.0/include",
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include",
    "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include",
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks"
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
            j["command"] += f" -I{c}"
    
    with open(path, 'w') as f:
        f.write(dumps(jsonInfo))

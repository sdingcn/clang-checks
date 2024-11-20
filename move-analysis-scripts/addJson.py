from json import loads, dumps
import sys
import subprocess

def execute(cmd, i):
    result = subprocess.run(
        cmd,
        text = True,
        input = i,
        capture_output = True
    )
    return (result.returncode, result.stdout, result.stderr)

def get_search_paths():
    _, out, err = execute(["clang++", "-x", "c++", "-o", "temp.out", "-v", "-w", "-"], "int main() {}")
    lines = err.splitlines()
    search_paths = []
    inlist = False
    for line in lines:
        if line.startswith("#include <...> search starts here:"):
            inlist = True
        elif line.startswith("End of search list."):
            inlist = False
        else:
            if inlist:
                search_paths.append(line.split()[0])
    return search_paths

if (__name__ == "__main__"):
    if (len(sys.argv) != 2):
        print("usage: python3 addJson <path-to-compile-comands.json>")
        sys.exit(1)
    path = sys.argv[1]
    with open(path) as f:
        jsonInfo = loads(f.read())
    search_paths = get_search_paths()
    for j in jsonInfo:
        for p in search_paths:
            j["command"] += " -I" + p
    
    with open(path, 'w') as f:
        f.write(dumps(jsonInfo))

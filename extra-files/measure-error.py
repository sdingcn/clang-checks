import json
import os.path
import sys
import subprocess

def read_code(s: str) -> ([str], str):
    _, tail1 = s.split("[[Erroneous calls]]")
    err_calls, tail2 = tail1.split("[[Summary]]")
    _, tail3 = tail2.split("[[Rewritten code]]")
    rewritten, _ = tail3.split("[[Resource consumption]]")
    ret1 = []
    for call in err_calls.splitlines():
        call = call.strip()
        if call:
            ret1.append(call)
    return (ret1, rewritten.strip())

def compose(main: str, call: str) -> str:
    lines = main.splitlines()
    lines.insert(-2, "struct S {};")
    lines.insert(-1, call)
    return "\n".join(lines)

def make_absolute(file, directory):
    if os.path.isabs(file):
        return file
    return os.path.abspath(os.path.join(directory, file))

def execute(cmd, i) -> str:
    return subprocess.run(
        cmd,
        text = True,
        input = i,
        stdout = subprocess.PIPE,
        stderr = subprocess.STDOUT
    ).stdout

def approx(a: int, b: int) -> bool:
    if a == 0 and b != 0:
        return False
    if a != 0 and b == 0:
        return False
    if a == 0 and b == 0:
        return True
    return abs(a - b) / min(a, b) < 0.05

def get_callee_name(call: str) -> str:
    return call.split("(")[0]

if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(f"Usage: python3 {sys.argv[0]} <origin> <output>")
    with open(sys.argv[1], "r") as f1:
        origin = f1.read().strip()
    with open(sys.argv[2], "r") as f2:
        err_calls, rewritten = read_code(f2.read())
    for call in err_calls:
        origin_err = execute(["clang++", "-x", "c++", "-w", "-std=c++20", "-"], compose(origin, call))
        rewritten_err = execute(["clang++", "-x", "c++", "-w", "-std=c++20", "-"], compose(rewritten, call))
        or_len = len(origin_err.splitlines())
        re_len = len(rewritten_err.splitlines())
        if not approx(or_len, re_len):
          name = get_callee_name(call)
          print(origin_err)
          print("\n\n\n\n\n")
          print(rewritten_err)
          print("\n\n\n\n\n")
          print(f"name = {name}, origin_err = {or_len}, rewritten_err = {re_len}")
          print("\n\n\n\n\n")
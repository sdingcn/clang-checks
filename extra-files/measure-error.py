import sys
import subprocess

def read_code(s: str) -> ([str], str, str):
    _, tail1 = s.split("[-[Erroneous calls]-]")
    err_calls, tail2 = tail1.split("[-[Summary]-]")
    summary, tail3 = tail2.split("[-[Rewritten code]-]")
    rewritten, _ = tail3.split("[-[Resource consumption]-]")
    ret1 = []
    for call in err_calls.splitlines():
        call = call.strip()
        if call:
            ret1.append(call)
    return (ret1, rewritten.strip(), summary.strip())

def compose(main: str, call: str) -> str:
    lines = main.splitlines()
    lines.insert(-2, "struct S {};")
    lines.insert(-1, "S s;")
    lines.insert(-1, call)
    return "\n".join(lines)

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
    if len(sys.argv) < 3:
        sys.exit(f"Usage: python3 {sys.argv[0]} <origin> <output> [name-prefix]")
    print("[-[Starting script]-]")
    with open(sys.argv[1], "r") as f1:
        origin = f1.read().strip()
    with open(sys.argv[2], "r") as f2:
        err_calls, rewritten, summary = read_code(f2.read())
    with open("rewritten.cc", "w") as f3:
        f3.write(rewritten)
    if len(sys.argv) == 4:
        err_calls = [ec for ec in err_calls if ec.startswith(sys.argv[3])]
    n = len(err_calls)
    or_ne_tot, re_ne_tot, cnt = 0, 0, 0
    err_lst = []
    for (i, call) in enumerate(err_calls):
        origin_err = execute(["clang++", "-x", "c++", "-w", "-std=c++20", "-"], compose(origin, call))
        rewritten_err = execute(["clang++", "-x", "c++", "-w", "-std=c++20", "-"], compose(rewritten, call))
        err_lst.append((i + 1, origin_err, rewritten_err))
        or_ne = len(origin_err.splitlines())
        re_ne = len(rewritten_err.splitlines())
        name = get_callee_name(call)
        print(f"{i + 1}/{n}: or_ne = {or_ne},\tre_ne = {re_ne},\tname = {name}")
        or_ne_tot += or_ne
        re_ne_tot += re_ne
        cnt += 1
    if cnt > 0:
        print(f"or_ne_avg = {round(or_ne_tot / cnt, 3)},\tre_ne_avg = {round(re_ne_tot / cnt, 3)}")
    print(summary)
    with open("result", "w") as f4:
        for num, or_err, re_err in err_lst:
            f4.write(f"[-[{num}]-]{{{{{{\n")
            f4.write(or_err + "\n")
            f4.write("[>----------<]\n")
            f4.write(re_err + "\n")
            f4.write("}}}}}}\n\n\n\n\n\n")

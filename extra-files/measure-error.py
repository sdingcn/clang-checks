import sys
import subprocess

def read_code(s: str) -> ([str], str):
    _, tail1 = s.split("[-[Erroneous calls]-]")
    err_calls, tail2 = tail1.split("[-[Summary]-]")
    _, tail3 = tail2.split("[-[Rewritten code]-]")
    rewritten, _ = tail3.split("[-[Resource consumption]-]")
    ret1 = []
    for call in err_calls.splitlines():
        call = call.strip()
        if call:
            ret1.append(call)
    return (ret1, rewritten.strip())

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
    if len(sys.argv) != 3:
        sys.exit(f"Usage: python3 {sys.argv[0]} <origin> <output>")
    with open(sys.argv[1], "r") as f1:
        origin = f1.read().strip()
    with open(sys.argv[2], "r") as f2:
        err_calls, rewritten = read_code(f2.read())
    err_calls = err_calls[:5]
    n = len(err_calls)
    or_len_tot, re_len_tot, cnt = 0, 0, 0
    err_lst = []
    for (i, call) in enumerate(err_calls):
        origin_err = execute(["clang++", "-x", "c++", "-w", "-std=c++20", "-"], compose(origin, call))
        rewritten_err = execute(["clang++", "-x", "c++", "-w", "-std=c++20", "-"], compose(rewritten, call))
        err_lst.append((origin_err, rewritten_err))
        or_len = len(origin_err.splitlines())
        re_len = len(rewritten_err.splitlines())
        if not approx(or_len, re_len):
            name = get_callee_name(call)
            print(f"{i + 1}/{n}: or_len = {or_len},\tre_len = {re_len},\tname = {name}")
            or_len_tot += or_len
            re_len_tot += re_len
            cnt += 1
    if cnt > 0:
        print(f"or_len_avg = {round(or_len_tot / cnt, 3)},\tre_len_avg = {round(re_len_tot / cnt, 3)}")
    with open("result", "w") as f3:
        for or_err, re_err in err_lst:
            f3.write("{{{{{{\n")
            f3.write(or_err + "\n")
            f3.write("\n\n\n------\n\n\n")
            f3.write(re_err + "\n")
            f3.write("}}}}}}\n")
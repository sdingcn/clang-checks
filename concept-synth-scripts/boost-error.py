import sys
import os.path
from typing import List, Tuple
from utils import *

def read_synth(s: str) -> Tuple[List[str], str, str]:
    _, tail1 = s.split("[-[Erroneous calls]-]")
    err_calls, tail2 = tail1.split("[-[Summary]-]")
    summary, tail3 = tail2.split("[-[Rewritten code]-]")
    rewritten, _ = tail3.split("[-[Resource consumption]-]")
    return ([ec.strip() for ec in err_calls.splitlines() if ec.strip()],
        rewritten.strip(), summary.strip())

def compose(main: str, call: str) -> str:
    lines = main.splitlines()
    lines.insert(-2, "struct S {};")
    lines.insert(-1, "S s;")
    lines.insert(-1, call)
    return "\n".join(lines)

def run_compiler(compiler: str, code: str) -> Tuple[int, str, str]:
    return execute([compiler, "-x", "c++", "-std=c++20", "-w", "-"], code)

def get_callee_name(call: str) -> str:
    return call.split("(")[0]

def classify(lst: List[int]) -> List[Tuple[int, int, int]]:
    results = []
    lst.sort()
    n = len(lst)
    i = 0
    start = 0
    step = 50
    cnt = 0
    while i < n:
        if lst[i] < start + step:
            cnt += 1
            i += 1
        else:
            results.append((start, start + step, cnt))
            start = start + step
            cnt = 0
    if cnt > 0:
        results.append((start, start + step, cnt))
    return results

def run_list(compiler: str, prep: str, rewritten: str, err_calls: List[str], errors_path) -> None:
    print(f"[-[Starting error measurement {compiler}]-]")
    n = len(err_calls)
    pr_ne_lst, re_ne_lst = [], []
    err_lst = []
    for (i, call) in enumerate(err_calls):
        pr_err = run_compiler(compiler, compose(prep, call))[2]
        re_err = run_compiler(compiler, compose(rewritten, call))[2]
        err_lst.append((i + 1, pr_err, re_err))
        pr_ne = len(pr_err.splitlines())
        re_ne = len(re_err.splitlines())
        print(f"{i + 1}/{n}: pr_ne = {pr_ne},\tre_ne = {re_ne},\tname = {get_callee_name(call)}")
        pr_ne_lst.append(pr_ne)
        re_ne_lst.append(re_ne)
    with open(errors_path, "w") as f:
        for i, pr_err, re_err in err_lst:
            f.write(f"[-[{i}]-]{{{{{{\n")
            f.write(pr_err + "\n")
            f.write("[>------------------------------<]\n")
            f.write(re_err + "\n")
            f.write("}}}}}}\n\n\n\n\n\n")
        if n > 0:
            f.write(f"pr_ne_avg = {round(sum(pr_ne_lst) / n, 3)},\t")
            f.write(f"re_ne_avg = {round(sum(re_ne_lst) / n, 3)}\n")
            f.write("[-[pr_ne_lst distribution]-]\n")
            pr_intervals = classify(pr_ne_lst)
            for begin, end, count in pr_intervals:
                f.write(f"[{begin}, {end}): {count}\n")
            f.write("[-[re_ne_lst distribution]-]\n")
            re_intervals = classify(re_ne_lst)
            for begin, end, count in re_intervals:
                f.write(f"[{begin}, {end}): {count}\n")

if __name__ == '__main__':
    for compiler in ["clang++"]:
        src_path = "boost-src.cc"
        prep_path = os.path.join("boost-error", compiler + "-prep.cc")
        result_path = os.path.join("boost-error", compiler + "-result.txt")
        rewritten_path = os.path.join("boost-error", compiler + "-rewritten.cc")
        name_prefix = "boost"
        errors_path = os.path.join("boost-error", compiler + "-errors.txt")

        prep = preprocess(compiler, src_path, ["../../boost_1_84_0"]) # hardcoded boost path
        with open(prep_path, "w") as f1:
            f1.write(prep)
        result = synth(prep_path)
        with open(result_path, "w") as f2:
            f2.write(result)
        err_calls, rewritten, _ = read_synth(result)
        with open(rewritten_path, "w") as f3:
            f3.write(rewritten)
        err_calls = [err_call for err_call in err_calls if err_call.startswith(name_prefix)]
        run_list(compiler, prep, rewritten, err_calls, errors_path)

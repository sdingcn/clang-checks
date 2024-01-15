import sys
import subprocess
from typing import List, Tuple

def execute(cmd: List[str], i: str) -> Tuple[int, str, str]:
    result = subprocess.run(
        cmd,
        text = True,
        input = i,
        capture_output = True
    )
    return (result.returncode, result.stdout, result.stderr)

def preprocess(compiler: str, fpath: str, ipaths: List[str]) -> str:
    cmd = [compiler, "-std=c++20", "-E", "-P", fpath]
    for ipath in ipaths:
        cmd.append("-I" + ipath)
    result = execute(cmd)
    if result[0] != 0:
        sys.exit(f"Error: {compiler} failed on {fpath} during preprocessing")
    return result[1]

def synth(fpath: str) -> str:
    result = execute(["../build/bin/concept-synth", fpath, "--", "-std=c++20"])
    if result[0] != 0:
        sys.exit(f"Error: concept-synth failed on {fpath}")
    return result[1]

def read_synth(s: str) -> Tuple[List[str], str, str]:
    _, tail1 = s.split("[-[Erroneous calls]-]")
    err_calls, tail2 = tail1.split("[-[Summary]-]")
    summary, tail3 = tail2.split("[-[Rewritten code]-]")
    rewritten, _ = tail3.split("[-[Resource consumption]-]")
    return ([ec.strip() for ec in err_calls.splitlines() if ec.strip()], rewritten.strip(), summary.strip())

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

def run_list(compiler: str, prep: str, rewritten: str, err_calls: List[str]) -> None:
    print(f"[-[Starting error measurement {compiler}]-]")
    n = len(err_calls)
    pr_ne_tot, re_ne_tot = 0, 0
    err_lst = []
    for (i, call) in enumerate(err_calls):
        pr_err = run_compiler(compiler, compose(prep, call))[2]
        re_err = run_compiler(compiler, compose(rewritten, call))[2]
        err_lst.append((i + 1, pr_err, re_err))
        pr_ne = len(pr_err.splitlines())
        re_ne = len(re_err.splitlines())
        print(f"{i + 1}/{n}: pr_ne = {pr_ne},\tre_ne = {re_ne},\tname = {get_callee_name(call)}")
        pr_ne_tot += pr_ne
        re_ne_tot += re_ne
    if n > 0:
        print(f"pr_ne_avg = {round(pr_ne_tot / n, 3)},\tre_ne_avg = {round(re_ne_tot / n, 3)}")
    with open(compiler + "-errs.txt", "w") as f:
        for i, pr_err, re_err in err_lst:
            f.write(f"[-[{i}]-]{{{{{{\n")
            f.write(pr_err + "\n")
            f.write("[>------------------------------<]\n")
            f.write(re_err + "\n")
            f.write("}}}}}}\n\n\n\n\n\n")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(f"Usage: python3 {sys.argv[0]} <fpath> [name-prefix]")
    src_path = sys.argv[1]
    for compiler in ["clang++-17", "g++-13"]:
        print(f"[-[Starting script on {compiler}]-]")
        prep_path = compiler + "-prep.cc"
        with open(prep_path, w) as f1:
            prep = preprocess(compiler, src_path, [])
            f1.write(prep)
        err_calls, rewritten, summary  = read_synth(synth(prep_path))
        print(summary)
        rewritten_path = compiler + "-rewritten.cc"
        with open(rewritten_path, w) as f2:
            f2.write(rewritten + "\n")
        if len(sys.argv) == 3:
            err_calls = [ec for ec in err_calls if ec.startswith(sys.argv[2])]
        run_list(compiler, prep, rewritten, err_calls)

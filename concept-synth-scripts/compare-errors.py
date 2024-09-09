import os.path
import subprocess
import sys
from typing import List, Tuple, Union

def execute(cmd: List[str], i: Union[None, str] = None) -> Tuple[int, str, str]:
    result = subprocess.run(
        cmd,
        text = True,
        input = i,
        capture_output = True
    )
    return (result.returncode, result.stdout, result.stderr)

def call_preprocessor(clang_path: str, fpath: str, ipaths: List[str]) -> str:
    cmd = [clang_path, "-std=c++20", "-E", "-P", fpath]
    for ipath in ipaths:
        cmd.append("-I" + ipath)
    result = execute(cmd)
    if result[0] != 0:
        sys.exit(f"Error: {cmd} failed")
    return result[1]

def call_synthesizer(synthesizer_path: str, fpath: str) -> str:
    cmd = [synthesizer_path, fpath, "--", "-std=c++20"]
    result = execute(cmd)
    if result[0] != 0:
        sys.exit(f"Error: {cmd} failed")
    return result[1]

def cut_synthesizer_output_section(output: str, section: str) -> str:
    section_header = f'[-[{section}]-]'
    start = output.find(section_header) + len(section_header)
    end = output.find('[-[]-]', start)
    return output[start:end].strip()

def compose_invalid_code(main: str, call: str) -> str:
    lines = main.splitlines()
    lines.insert(-2, "struct S {};")  # insert before
    lines.insert(-1, "S s;")
    lines.insert(-1, call)
    return "\n".join(lines)

def get_error_message(clang_path: str, code: str) -> str:
    cmd = [clang_path, "-x", "c++", "-std=c++20", "-w", "-"]
    result = execute(cmd, code)
    if result[0] == 0:
        sys.exit(f"Error: {cmd} didn't fail as expected")
    return result[2]

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

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: python3 {sys.argv[0]} <c++-source-file>")
    paths = {
        "clang": "clang++",
        "synthesizer": "../build/bin/concept-synthesizer",
        "source": sys.argv[1],
        "preprocessed": sys.argv[1] + ".preprocessed.cc",
        "constrained": sys.argv[1] + ".constrained.cc",
    }
    preprocessed_code = call_preprocessor(paths["clang"], paths["source"], [])
    with open(paths["preprocessed"], "w") as f:
        f.write(preprocessed_code)
    synthesizer_output = call_synthesizer(paths["synthesizer"], paths["preprocessed"])
    constrained_code = cut_synthesizer_output_section(synthesizer_output, "Constrained code")
    invalid_calls = [
        ic
        for ic in cut_synthesizer_output_section(synthesizer_output, "Invalid calls").splitlines()
        if ic
    ]
    statistics = cut_synthesizer_output_section(synthesizer_output, "Statistics")
    resource_consumption = cut_synthesizer_output_section(synthesizer_output, "Resource consumption")
    print("#### Statistics:")
    print(statistics)
    print("#### Resource consumption:")
    print(resource_consumption)
    with open(paths["constrained"], "w") as f:
        f.write(constrained_code)
    print("#### Error reduction measurement:")
    n = len(invalid_calls)
    original_error_lengths = []
    constrained_error_lengths = []
    for (i, call) in enumerate(invalid_calls):
        error1 = get_error_message(paths["clang"], compose_invalid_code(preprocessed_code, call))
        error2 = get_error_message(paths["clang"], compose_invalid_code(constrained_code, call))
        original_error_length = len(error1.splitlines())
        constrained_error_length = len(error2.splitlines())
        print(f"{i + 1}/{n}: original = {original_error_length}\t"
              f"constrained = {constrained_error_length}\t"
              f"name = {get_callee_name(call)}")
        original_error_lengths.append(original_error_length)
        constrained_error_lengths.append(constrained_error_length)
    if n > 0:
        print(f"original average = {round(sum(original_error_lengths) / n, 3)}")
        print(f"constrained average = {round(sum(constrained_error_lengths) / n, 3)}")
        print("original distribution:")
        original_intervals = classify(original_error_lengths)
        for begin, end, count in original_intervals:
            print(f"[{begin}, {end}) = {count}")
        print("constrained distribution:")
        constrained_intervals = classify(constrained_error_lengths)
        for begin, end, count in constrained_intervals:
            print(f"[{begin}, {end}) = {count}")

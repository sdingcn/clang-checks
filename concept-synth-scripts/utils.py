import subprocess
import sys
from typing import List, Tuple, Union

def execute(cmd: List[str], i: Union[None, str]) -> Tuple[int, str, str]:
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
    result = execute(cmd, None)
    if result[0] != 0:
        sys.exit(f"Error: {compiler} failed on {fpath} during preprocessing")
    return result[1]

def synth(fpath: str) -> str:
    result = execute(["../build/bin/concept-synth", fpath, "--", "-std=c++20"], None)
    if result[0] != 0:
        sys.exit(f"Error: concept-synth failed on {fpath}")
    return result[1]

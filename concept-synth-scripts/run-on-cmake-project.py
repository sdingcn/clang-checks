import json
import os.path
import sys
from utils import *
from typing import List, Tuple
import subprocess
import re
from typing import List


def make_absolute(file: str, directory: str) -> str:
    if os.path.isabs(file):
        return file
    return os.path.abspath(os.path.join(directory, file))

def callback(result: Tuple[int, str, str]) -> None:
    if (result[0] == 0):
        pass
    pass

def run_cmake_tests(build_dir: str) -> None:
    # Run CMake tests
    cmake_command = ['cmake', '--build', build_dir, '--target', 'test']
    subprocess.run(cmake_command, check=True)

def parse_test_output(output: str) -> List[float]:
    # Parse test output to extract individual test runtimes
    test_runtimes = []
    pattern = re.compile(r'Test time = (\d+\.\d+)s')
    matches = re.findall(pattern, output)
    for match in matches:
        test_runtimes.append(float(match))
    return test_runtimes

def calculate_average_runtime(test_runtimes: int) -> str:
    # Calculate the average runtime of all tests
    if test_runtimes:
        return sum(test_runtimes) / len(test_runtimes)
    else:
        return 0


def run_test_integration(build_dir: str) -> int:
    run_cmake_tests(build_dir)

    # Read test output
    with open('Testing/Temporary/LastTest.log', 'r') as f:
        test_output = f.read()

    # Parse test output to extract runtimes
    test_runtimes = parse_test_output(test_output)

    # Calculate average runtime
    return calculate_average_runtime(test_runtimes)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(f'Usage: python3 {sys.argv[0]} <build-path>')
    build_path = sys.argv[1]
    database = json.load(open(os.path.join(build_path, 'compile_commands.json')))
    files = set(
        # according to the specification (https://clang.llvm.org/docs/JSONCompilationDatabase.html)
        # paths in the 'file' field must be either absolute or relative to the 'directory' field
        [make_absolute(entry['file'], entry['directory']) for entry in database]
    )
    for file in files:
        callback(execute(['../build/bin/concept-synth', '-p', build_path, file]))

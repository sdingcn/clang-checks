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


def main():
    build_dir = 'path_to_build_directory'
    run_cmake_tests(build_dir)

    # Read test output
    with open('Testing/Temporary/LastTest.log', 'r') as f:
        test_output = f.read()

    # Parse test output to extract runtimes
    test_runtimes = parse_test_output(test_output)

    # Calculate average runtime
    average_runtime = calculate_average_runtime(test_runtimes)
    print("Average Runtime of Tests:", average_runtime, "seconds")

if __name__ == "__main__":
    main()
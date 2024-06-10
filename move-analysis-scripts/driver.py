import random
import subprocess
import sys
from typing import List, Tuple, Union

# assumptions on the Clang-based move checker (to be implemented):
# without -fix, the move checker dumps all feasible moves from the target C++ project into "moves.tmp"
#     see scan_moves() below
# with -fix, the move checker applies only the moves in "moves.tmp" to the target C++ project
#     see apply_moves() below

# the format of "moves.tmp":
#     each line is a pair of natural numbers denoting the row and column of move
#     the row and columns are based on the original C++ file (without any inserted new moves)

def execute(cmd: List[str], i: Union[None, str]) -> Tuple[int, str, str]:
    """
    execute a command and return the outputs, etc.
    """
    result = subprocess.run(
        cmd,
        text = True,
        input = i,
        capture_output = True
    )
    return (result.returncode, result.stdout, result.stderr)

def scan_moves(project_path: str) -> None:
    """
    run the move checker (without -fix) to dump all feasible moves from the target C++ project into "moves.tmp"
    """
    execute([
        '../clang-tools-extra/clang-tidy/tool/run-clang-tidy.py',
        '-clang-tidy-binary',
        '../build/bin/clang-tidy',
        '-p',
        project_path,
        '-checks="-*,performance-missing-moves"',
        '-quiet'
    ])

def apply_moves(project_path: str) -> None:
    """
    run the move checker (with -fix) to apply only the moves in "moves.tmp" to the target C++ project
    """
    execute([
        '../clang-tools-extra/clang-tidy/tool/run-clang-tidy.py'
        '-fix',
        '-clang-tidy-binary',
        '../build/bin/clang-tidy',
        '-clang-apply-replacements-binary',
        '../build/bin/clang-apply-replacements',
        '-p',
        project_path,
        '-checks="-*,performance-missing-moves"',
        '-quiet'
    ])

def read_moves() -> List[Tuple[int, int]]:
    """
    read moves from "moves.tmp"
    """
    moves = []
    with open('moves.tmp', 'r') as f:
        for location in f.readlines():
            line, column = location.split()
            moves.append((int(line), int(column)))
    return moves

def write_moves(moves: List[Tuple[int, int]]) -> None:
    """
    write moves into "moves.tmp" (overwrite existing contents)
    """
    with open('moves.tmp', 'w') as f:
        for line, column in moves:
            f.write(f'{line} {column}\n')

def get_time(project_path: str) -> int:
    """
    get the execution time of the target C++ project's benchmark (maybe running several times and getting the average)
    """
    # TODO (this is the place where I call your testing time script)
    return 0

def revert_changes(project_path: str) -> None:
    """
    revert the target C++ project back to the original version
    """
    # TODO (perhaps this is just a "git restore")

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(f'Usage: python3 {sys.argv[0]} <project-path>')

    # the target C++ project's build path (the path containing compile_commands.json)
    project_path = sys.argv[1]

    # the original execution time of the target C++ project's benchmark
    original_time = get_time(project_path)

    # find all feasible moves and write them into "moves.tmp"
    scan_moves(project_path)

    # try to trim all feasible moves down to <= 10 effective ones by binary cut
    while True:
        moves = read_moves()
        n = len(moves)
        if n <= 10:
            # if there are <= 10 remaining moves, stop
            break
        else:
            best_time_improvement = 0
            best_half_moves = []
            # otherwise, try (3 times or whatever) to find (n / 2) effective moves
            for i in range(3):
                random.shuffle(moves)
                # try the left half
                left_moves = moves[:n / 2]
                write_moves(left_moves) # write to "moves.tmp"
                apply_moves(project_path)
                left_time = get_time(project_path)
                if best_time_improvement < original_time - left_time:
                    best_time_improvement = original_time - left_time
                    best_half_moves = left_moves
                revert_changes(project_path)
                # try the right half
                right_moves = moves[n / 2:]
                write_moves(right_moves) # write to "moves.tmp"
                apply_moves(project_path)
                right_time = get_time(project_path)
                if best_time_improvement < original_time - right_time:
                    best_time_improvement = original_time - right_time
                    best_half_moves = right_moves
                revert_changes(project_path)
            if best_time_improvement == 0:
                # failure: revert back and stop
                write_moves(moves)
                print('Stopped trimming: unable to reduce moves further')
                break
            else:
                print(
                    f'Succeeded one round with time improvement {best_time_improvement} '
                    f'on {len(best_half_moves)} moves; writing back to "moves.tmp"...'
                )
                write_moves(best_half_moves)
                # continue to the next iteration of the trimming loop
    
    # the effective moves (if any) will be available in "moves.tmp"
    # and you can either run the move checker with -fix to apply them or manually inspect them

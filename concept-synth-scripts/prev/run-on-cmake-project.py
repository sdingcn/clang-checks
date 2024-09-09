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
        execute(['../build/bin/concept-synthesizer', '-p', build_path, file])

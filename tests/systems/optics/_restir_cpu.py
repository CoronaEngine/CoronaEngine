"""Shared standalone CPU boundary harness; this is not a GPU/EDSL build."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[3]
TEMP = ROOT / '.workspace/tmp/restir-performance/tests'


def balanced(source, start):
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return index + 1
    raise AssertionError('Unbalanced production source')


def body(source, token):
    start = source.index(token)
    opening = source.index('{', start)
    return source[start:opening], source[opening:balanced(source, start)]


def compile_run(program, name, include_dirs=()):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise SkipTest('Standalone ReSTIR regression needs g++ or clang++')
    TEMP.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=name+'-', dir=TEMP) as directory:
        source, binary = Path(directory)/'test.cpp', Path(directory)/'test.exe'
        source.write_bytes(program.encode('utf-8'))
        built = subprocess.run([compiler, '-std=c++20', '-O0',
                                *('-I'+str(p) for p in include_dirs), str(source), '-o', str(binary)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.startswith('PASS '), result.stdout
        print(result.stdout.strip())

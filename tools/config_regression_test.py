#!/usr/bin/env python3
"""Compile and run config numeric-boundary regressions (no ROM required).

Requires SDL3 headers and nlohmann-json; --json-include (or TMC_JSON_INCLUDE)
overrides discovery, see regression_build.py.
Runs with UBSan, including float-to-integer overflow checks.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from regression_build import GC_SECTIONS, cxx, json_cflags, sdl3_cflags

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json-include', type=Path)
    args = parser.parse_args()
    json_flags = ['-I' + str(args.json_include)] if args.json_include else json_cflags()
    command = cxx() + [
        '-std=c++17', '-Iport', '-ffunction-sections', '-fdata-sections',
        '-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all',
    ] + sdl3_cflags() + json_flags
    with tempfile.TemporaryDirectory(prefix='tmc-config-test-') as directory:
        tmp = Path(directory)
        binary = tmp / 'config-test'
        subprocess.run(command + ['port/port_runtime_config.cpp', 'tools/tests/runtime_config.cpp',
                                 GC_SECTIONS, '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary), str(tmp / 'config.json')], check=True)


if __name__ == '__main__':
    main()

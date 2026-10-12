#!/usr/bin/env python3
"""Compile and run the issue #3 QoL toggle state checks in port/port_qol.c (no ROM required)."""
from pathlib import Path
import subprocess
import tempfile

from regression_build import GC_SECTIONS, cc, sdl3_cflags

ROOT = Path(__file__).resolve().parent.parent


def main():
    flags = ['-std=gnu11', '-DPC_PORT', '-I.', '-Iinclude', '-Iport', '-Wno-multichar',
             '-ffunction-sections', '-fdata-sections', GC_SECTIONS] + sdl3_cflags()
    with tempfile.TemporaryDirectory(prefix='tmc-qol-test-') as directory:
        binary = Path(directory) / 'qol-toggles'
        subprocess.run(cc() + flags + ['tools/tests/qol_toggles.c', '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True)
    print('qol toggle regression checks passed')


if __name__ == '__main__':
    main()

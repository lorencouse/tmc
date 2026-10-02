#!/usr/bin/env python3
"""Run native graphics allocator regressions against production code; no ROM needed."""
from pathlib import Path
import subprocess
import tempfile

from regression_build import GC_SECTIONS, cc

ROOT = Path(__file__).resolve().parent.parent


def main():
    command = cc() + [
        '-std=gnu11', '-DPC_PORT', '-DMULTI_REGION', '-DUSA', '-DENGLISH',
        '-I.', '-Iinclude', '-Iport', '-ffunction-sections', '-fdata-sections',
        'tools/tests/gfx_slots.c', 'src/vram.c', GC_SECTIONS,
    ]
    with tempfile.TemporaryDirectory(prefix='tmc-gfx-test-') as directory:
        binary = Path(directory) / 'gfx-test'
        subprocess.run(command + ['-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()

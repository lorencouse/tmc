#!/usr/bin/env python3
"""Check the production enemy home initializer against PC room-spawn layout."""
from pathlib import Path
import subprocess
import tempfile

from regression_build import GC_SECTIONS, cc

ROOT = Path(__file__).resolve().parent.parent


def main():
    with tempfile.TemporaryDirectory(prefix='tmc-enemy-home-') as directory:
        for region in ('USA', 'EU', 'JP'):
            binary = Path(directory) / region
            subprocess.run(cc() + [
                '-std=gnu11', '-DPC_PORT', '-D' + region, '-I.', '-Iinclude', '-Iport',
                '-ffunction-sections', '-fdata-sections', 'tools/tests/enemy_home.c',
                'src/enemyUtils.c', GC_SECTIONS, '-o', str(binary),
            ], cwd=ROOT, check=True)
            subprocess.run([str(binary)], check=True)
            print(region + ': PASS', flush=True)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Run the real fusion offer scanner against bounded and malformed save cursors."""
from pathlib import Path
import subprocess
import tempfile

from regression_build import GC_SECTIONS, cc

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='tmc-fuser-test-') as directory:
    output = str(Path(directory) / 'fuser_cursor')
    # PC headers already provide the map constants. The unrelated map tables
    # in common.c do not need an extracted-ROM header for this scanner test.
    (Path(directory) / 'assets').mkdir()
    (Path(directory) / 'assets/map_offsets.h').write_text('/* supplied by port_offset_USA.h */\n')
    subprocess.run(cc() +
                   ['-std=gnu11', '-DPC_PORT', '-DMULTI_REGION', '-DUSA', '-I.', '-Iinclude',
                    '-Iport', '-I' + directory, '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast',
                    '-ffunction-sections', '-fdata-sections', GC_SECTIONS,
                    'tools/tests/fuser_cursor.c', '-o', output], cwd=ROOT, check=True)
    subprocess.run([output], cwd=ROOT, check=True)
    print('fuser_cursor: PASS')

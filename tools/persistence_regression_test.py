#!/usr/bin/env python3
"""Exercise production save, quicksave and extra-item-slot code in scratch directories."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    flags = ['-std=gnu11', '-DPC_PORT', '-I.', '-Iinclude', '-Iport', '-Iport/ppu/include', '-DMODE1_GBA_WIDTH=240',
             '-Wno-multichar', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
    failed = False
    with tempfile.TemporaryDirectory(prefix='tmc-persistence-test-') as directory:
        tests = [('save_persistence', region, ['retail', 'legacy', 'legacy-default', 'backup', 'switch'])
                 for region in ('USA', 'EU', 'JP')]
        # quicksave_entities (from upstream) tests upstream's pointer-relocating
        # quicksave; the fork snapshots whole globals from the generated
        # gPortStateRegions table instead, so it cannot link here. Save-state
        # round trips are covered by the TMC_REPRO_* cross-process harness.
        tests += [('softslot_ownership', 'USA', ['ownership'])]
        for name, region, cases in tests:
            binary = Path(directory) / f'{name}-{region}'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + flags + ['-D' + region] +
                           [f'tools/tests/{name}.c', '-o', str(binary)], cwd=ROOT, check=True)
            for case in cases:
                scratch = Path(directory) / f'{name}-{region}-{case}'
                scratch.mkdir()
                env = os.environ.copy()
                env.pop('TMC_SAVE_RETAIL_LAYOUT', None)
                env.pop('TMC_SAVE_MIGRATE_LEGACY_FLAGS', None)
                result = subprocess.run([str(binary), case], cwd=scratch, env=env)
                failed |= result.returncode != 0
                print(f'{name}/{region}/{case}: {"PASS" if result.returncode == 0 else "FAIL"}', flush=True)
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()

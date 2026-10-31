#!/usr/bin/env python3
"""Validate the small NuttX fixes against the pinned Git source, without editing it."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = '76354c637858ecb0aa4601629327acb6f44a26bb'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nuttx-root', type=Path, required=True)
    options = parser.parse_args()
    paths = ['drivers/rpmsg/rpmsg_procfs.c', 'libs/libc/netdb/lib_dnsquery.c']
    with tempfile.TemporaryDirectory(prefix='zhaoxi-nuttx-') as directory:
        target = Path(directory)
        for path in paths:
            data = subprocess.check_output(['git', '-C', str(options.nuttx_root), 'show', BASE + ':' + path])
            output = target / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
        for number in (22, 24):
            files = list((ROOT / 'patches').glob(f'{number:04d}-nuttx-*.patch'))
            if len(files) != 1: raise RuntimeError('Expected one NuttX patch: ' + str(number))
            for flags in (['--check'], []):
                subprocess.run(['git', 'apply', *flags, str(files[0])], cwd=target, check=True)
        for path in paths:
            if (target/path).read_text() != (options.nuttx_root/path).read_text():
                raise RuntimeError('Build source differs from patch: ' + path)
    print(json.dumps({'base':BASE, 'apply_and_source_comparison':'passed', 'board_operations':False}))

if __name__ == '__main__': main()

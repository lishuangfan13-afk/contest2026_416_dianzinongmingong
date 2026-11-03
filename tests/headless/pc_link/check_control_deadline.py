#!/usr/bin/env python3
"""Run actual PC control C with deterministic socket/time doubles; no network."""
from pathlib import Path
import argparse
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cjson', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    actual = (root / 'app/zhaoxi/pc_control.c').read_text()
    # Compile the complete implementation (dispatch, I/O and public entries),
    # replacing only platform headers with the host harness declarations.
    actual = actual[actual.index('extern int zhaoxi_main('):]
    template = Path(__file__).with_name('test_control_deadline.c').read_text()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-control-deadline-') as folder:
        folder = Path(folder)
        source = folder / 'test.c'
        source.write_text(template.replace('/* ACTUAL_PC_CONTROL */', actual))
        exe = folder / 'test'
        subprocess.run([
            'gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
            '-I' + str(args.cjson), '-I' + str(root / 'app/zhaoxi'),
            str(source), str(args.cjson / 'cJSON.c'), '-lm', '-o', str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == '__main__':
    main()

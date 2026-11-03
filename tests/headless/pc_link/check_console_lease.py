#!/usr/bin/env python3
"""Offline actual-C console lease tests; no serial, networking or firmware build.

Run with a POSIX host gcc/python3 (e.g. WSL Ubuntu):
  python3 tests/headless/pc_link/check_console_lease.py
Optional --apps points at the pristine apps tree matching patch 0074.
"""
from pathlib import Path
import argparse
import difflib
import re
import subprocess
import tempfile


def apply_patch(original, patch, refresh=False):
    lines = original.splitlines(keepends=True)
    result, cursor = [], 0
    hunks = re.split(r'(?m)^@@[^\n]*\n', patch)[1:]
    headers = re.findall(r'(?m)^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@.*$', patch)
    if not headers or len(hunks) != len(headers):
        raise AssertionError('missing/malformed patch hunks')
    for header, hunk in zip(headers, hunks):
        body = hunk.splitlines(keepends=True)
        assert all(line[:1] in (' ', '-', '+') for line in body), 'bad patch body'
        old = [line[1:] for line in body if line[0] in ' -']
        new = [line[1:] for line in body if line[0] in ' +']
        if refresh:
            matches = [i for i in range(cursor, len(lines) - len(old) + 1)
                       if lines[i:i + len(old)] == old]
            assert len(matches) == 1, 'patch context must match uniquely'
            start = matches[0]
        else:
            start = int(header[0]) - 1
            assert len(old) == int(header[1] or 1), 'incorrect old hunk count'
            assert len(new) == int(header[3] or 1), 'incorrect new hunk count'
            assert start >= cursor and lines[start:start + len(old)] == old, 'patch drift'
            assert int(header[2]) - 1 == len(result) + start - cursor, 'bad new hunk offset'
        result += lines[cursor:start] + new
        cursor = start + len(old)
    result += lines[cursor:]
    return ''.join(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--refresh-patch', action='store_true', help='normalize draft hunk headers only')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    apps = args.apps or root.parent / 'openvela-build/apps'
    original = (apps / 'system/readline/readline_fd.c').read_text()
    patch_path = root / 'patches/0074-apps-readline-console-lease-for-boot-ppp.patch'
    patch = patch_path.read_bytes()
    assert b'\r' not in patch, 'patch must use LF'
    patched = apply_patch(original, patch.decode(), args.refresh_patch)
    if args.refresh_patch:
        normalized = ''.join(difflib.unified_diff(
            original.splitlines(keepends=True), patched.splitlines(keepends=True),
            fromfile='a/system/readline/readline_fd.c', tofile='b/system/readline/readline_fd.c'))
        patch_path.write_bytes(normalized.encode())
        return
    template = Path(__file__).with_name('test_console_lease.c').read_text()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-console-lease-') as directory:
        folder = Path(directory)
        (folder / 'nuttx').mkdir()
        (folder / 'system').mkdir()
        (folder / 'nuttx/config.h').write_text('#ifndef TEST_NUTTX_CONFIG_H\n#define TEST_NUTTX_CONFIG_H\n'
            '#define FAR\n#define UNUSED(x) (void)(x)\n#define DEBUGASSERT assert\n'
            '#ifndef TEST_NO_ECHO\n#define CONFIG_READLINE_ECHO 1\n#endif\n#endif\n')
        (folder / 'system/readline.h').write_text('#include <stdio.h>\n')
        (folder / 'readline.h').write_text((apps / 'system/readline/readline.h').read_text())
        source = folder / 'test.c'
        source.write_text(template.replace('/* ACTUAL_CONSOLE_LEASE */',
            (root / 'app/zhaoxi/console_lease.c').read_text()).replace('/* ACTUAL_READLINE_FD */', patched))
        for mode in ('product', 'no-hook', 'partial-hook', 'no-echo'):
            exe = folder / ('test-' + mode)
            flags = {'product': [], 'no-hook': ['-DNO_PRODUCT_HOOK'],
                     'partial-hook': ['-DPARTIAL_PRODUCT_HOOK'], 'no-echo': ['-DTEST_NO_ECHO']}[mode]
            subprocess.run([args.cc, '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I' + str(folder), '-I' + str(root / 'app/zhaoxi'),
                str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)
            print('PASS actual C:', mode, flush=True)
    print('PASS strict patch context/counts and LF; all I/O is simulated', flush=True)


if __name__ == '__main__':
    main()

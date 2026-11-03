#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile actual morning SNTP/ticks/wait_fd with deterministic host mocks.

No target hardware, real sockets, DNS, network, or production writes are used.
Run with Python 3 and gcc on Linux/WSL; no third-party dependencies are needed.
"""
from pathlib import Path
import subprocess
import tempfile


def extract_function(source, signature):
    """Keep each production function verbatim, including its complete body."""
    start = source.index(signature)
    end = source.index('\n}', start) + len('\n}')
    return source[start:end]


def main():
    root = Path(__file__).resolve().parents[3]
    source = (root / 'app/zhaoxi/morning_net.c').read_text(encoding='utf-8')
    actual = '\n\n'.join(extract_function(source, signature) for signature in (
        'static int64_t ticks(void)',
        'static int wait_fd(int fd,short events,int64_t end)',
        'int morning_sntp(int64_t *epoch)',
    ))
    template = Path(__file__).with_name('test_sntp_deadline.c').read_text(encoding='utf-8')
    marker = '/* ACTUAL_MORNING_SNTP */'
    if template.count(marker) != 1:
        raise RuntimeError('expected exactly one production insertion marker')
    with tempfile.TemporaryDirectory(prefix='morning-sntp-deadline-') as folder:
        folder = Path(folder)
        test = folder / 'test.c'
        test.write_text(template.replace(marker, actual), encoding='utf-8')
        executable = folder / 'test'
        subprocess.run([
            'gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
            '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
            str(test), '-o', str(executable),
        ], check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=10)
    print('PASS: actual production ticks/wait_fd/morning_sntp; gcc Werror + UBSan', flush=True)


if __name__ == '__main__':
    main()

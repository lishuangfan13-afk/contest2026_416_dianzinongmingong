#!/usr/bin/env python3
"""Compile the real opt-in PPP TCP window helper against offline C fakes."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    args = parser.parse_args()
    path = args.sdk / 'src/infra/vela_tls.c'
    lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
    begin = [i for i, line in enumerate(lines) if 'ZHAOXI_PPP_RX_WINDOW_BEGIN' in line]
    end = [i for i, line in enumerate(lines) if 'ZHAOXI_PPP_RX_WINDOW_END' in line]
    if len(begin) != 1 or len(end) != 1 or begin[0] >= end[0]:
        raise SystemExit('Expected one ordered pair of PPP receive-window markers')
    actual = ''.join(lines[begin[0] + 1:end[0]])
    template = Path(__file__).with_name('test_ppp_rx_window.c').read_text(encoding='utf-8')
    marker = '/* ACTUAL_PPP_RX_WINDOW_HELPERS */'
    if template.count(marker) != 1:
        raise SystemExit('Expected exactly one harness insertion marker')
    with tempfile.TemporaryDirectory(prefix='zhaoxi-ppp-rx-window-') as directory:
        directory = Path(directory)
        source = directory / 'test.c'
        executable = directory / 'test_ppp_rx_window'
        source.write_text(template.replace(marker, actual), encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(source), '-o', str(executable)],
                       check=True)
        subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Build the drink command with the real scheduler, cJSON and message bus."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--cjson', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-drink-') as temp:
        executable = Path(temp) / 'test_drink'
        flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-DCONFIG_EXAMPLES_ZHAOXI=1', '-DCONFIG_ZHAOXI_BES_AUDIO=1',
                 '-DAGENT_DATA_DIR="' + str(Path(temp) / 'data') + '"']
        include = [ROOT / 'tests/headless/include', ROOT / 'app/zhaoxi', args.sdk / 'src',
                   args.sdk / 'include', args.cjson]
        sources = [ROOT / 'app/zhaoxi/drink.c', args.sdk / 'src/infra/cron_service.c',
                   args.sdk / 'src/core/message_bus.c', ROOT / 'tests/headless/test_drink.c',
                   args.cjson / 'cJSON.c']
        subprocess.run(['cc', *flags, *['-I' + str(p) for p in include],
                        '-include', str(ROOT / 'tests/headless/cron_support.h'),
                        *map(str, sources), '-Wl,--wrap=time', '-Wl,--wrap=cron_list_jobs', '-pthread', '-lm', '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()

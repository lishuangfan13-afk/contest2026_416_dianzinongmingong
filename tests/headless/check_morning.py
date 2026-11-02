#!/usr/bin/env python3
"""Exercise actual morning storage/policy with real files and write failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cjson', type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='zhaoxi-morning-') as temp:
    binary = Path(temp) / 'test'
    subprocess.run([
        'gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
        '-Wno-misleading-indentation', '-fsanitize=address,undefined',
        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-pthread',
        '-I'+str(ROOT/'tests/headless/include'), '-I'+str(ROOT/'app/zhaoxi'),
        '-I'+str(args.cjson), '-DAGENT_DATA_DIR="'+str(Path(temp)/'data')+'"',
        str(ROOT/'app/zhaoxi/morning_store.c'), str(ROOT/'tests/headless/test_morning_store.c'),
        str(args.cjson/'cJSON.c'), '-Wl,--wrap=write', '-Wl,--wrap=fsync',
        '-Wl,--wrap=rename', '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)

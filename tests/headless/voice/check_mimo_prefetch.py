#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual MiMo provider + SSE prefetch transaction regression; no network/hardware."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--workspace', type=Path, required=True)
args = parser.parse_args()
sdk = args.workspace / 'packages/ai_agent'
mbed = args.workspace / 'apps/crypto/mbedtls/mbedtls'
cjson = args.workspace / 'apps/netutils/cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='zhaoxi-mimo-prefetch-') as folder:
    temp = Path(folder)
    (temp / 'agent_compat.h').write_text('#pragma once\n#include <stdbool.h>\nbool agent_shutdown_requested(void);\n')
    (temp / 'agent_config.h').write_text('#pragma once\n')
    executable = temp / 'test'
    subprocess.run([
        'gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
        '-Wno-misleading-indentation', '-ffunction-sections', '-fdata-sections',
        '-Wl,--gc-sections', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-fno-omit-frame-pointer', '-I' + str(temp), '-I' + str(ROOT / 'app/zhaoxi'),
        '-I' + str(sdk / 'src'), '-I' + str(sdk / 'include'), '-I' + str(cjson),
        '-I' + str(mbed / 'include'),
        '-DMIMO_SOURCE="' + str(ROOT / 'app/zhaoxi/mimo_voice.c') + '"',
        '-DMIMO_SSE_SOURCE="' + str(ROOT / 'app/zhaoxi/mimo_sse.c') + '"',
        str(Path(__file__).with_name('test_mimo_prefetch.c')), str(cjson / 'cJSON.c'),
        str(mbed / 'library/base64.c'), str(mbed / 'library/constant_time.c'),
        '-o', str(executable),
    ], check=True, timeout=45)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    result = subprocess.run([str(executable)], env=env, text=True,
                            stdout=subprocess.PIPE, check=True, timeout=30)
    for line in result.stdout.splitlines():
        if line.startswith('PASS:'):
            print(line)

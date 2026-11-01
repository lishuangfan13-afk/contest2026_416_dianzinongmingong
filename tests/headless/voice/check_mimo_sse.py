#!/usr/bin/env python3
"""Compile the production SSE decoder with real cJSON and mbedTLS base64."""
from pathlib import Path
import argparse, subprocess, tempfile

ROOT=Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--workspace',required=True,type=Path)
args=parser.parse_args();ws=args.workspace.resolve()
cjson=ws/'apps/netutils/cjson/cJSON';mbed=ws/'apps/crypto/mbedtls/mbedtls'
with tempfile.TemporaryDirectory(prefix='mimo-sse-') as folder:
    exe=Path(folder)/'test_mimo_sse'
    subprocess.run(['gcc','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-Wl,--gc-sections',
        '-I'+str(ROOT/'app/zhaoxi'),'-I'+str(cjson),'-I'+str(mbed/'include'),'-I'+str(mbed/'library'),
        str(ROOT/'app/zhaoxi/mimo_sse.c'),str(Path(__file__).with_name('test_mimo_sse.c')),
        str(cjson/'cJSON.c'),str(mbed/'library/base64.c'),str(mbed/'library/constant_time.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)

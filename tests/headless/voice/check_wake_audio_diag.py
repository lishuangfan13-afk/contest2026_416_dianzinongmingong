#!/usr/bin/env python3
"""Verify the opt-in RAM diagnostic and its base64/CRC transfer with fake PCM."""
import argparse
import base64
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--mbedtls',required=True,type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='wake-audio-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-pie','-no-pie','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-I'+str(ROOT/'app/zhaoxi'),'-I'+str(a.mbedtls/'include'),
        str(ROOT/'app/zhaoxi/wake_audio_diag.c'),str(ROOT/'tests/headless/voice/test_wake_audio_diag.c'),
        str(a.mbedtls/'library/base64.c'),str(a.mbedtls/'library/constant_time.c'),
        str(a.mbedtls/'library/platform_util.c'),'-pthread','-o',str(exe)],check=True)
    output=subprocess.check_output([str(exe)],text=True)
    frames=re.findall(r'WAKEPCM_DATA at=(\d+) n=(\d+) crc=([a-f0-9]+) data=([A-Za-z0-9+/=]+)',output)
    expected=b''.join(struct.pack('<H',i*17) for i in range(600));received=b''
    for at,n,crc,data in frames:
        raw=base64.b64decode(data,validate=True)
        assert len(received)==int(at) and len(raw)==int(n) and zlib.crc32(raw)==int(crc,16)
        received+=raw
    assert received==expected
    assert f'WAKEPCM_BEGIN bytes=1200 crc={zlib.crc32(expected):08x}' in output
    print('PASS: diagnostic opt-in, bounds, one-shot freeze, clear and exact CRC/base64 round trip')

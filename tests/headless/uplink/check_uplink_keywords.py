#!/usr/bin/env python3
"""Build actual DSP/classifier with fake PCM; no real recordings or network."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='uplink-keywords-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-fno-pie','-no-pie','-I'+str(ROOT/'app/zhaoxi'),
        str(ROOT/'app/zhaoxi/uplink_keywords.c'),str(ROOT/'tests/headless/uplink/test_uplink_keywords.c'),
        '-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)

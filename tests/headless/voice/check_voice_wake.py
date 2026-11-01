#!/usr/bin/env python3
"""Compile the pure ASR wake-phrase gate; no ASR, microphone or network."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',required=True,type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='zhaoxi-wake-') as folder:
    exe=Path(folder)/'test'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(a.sdk/'src'),
        str(a.sdk/'src/voice/voice_wake.c'),str(Path(__file__).with_name('test_voice_wake.c')),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

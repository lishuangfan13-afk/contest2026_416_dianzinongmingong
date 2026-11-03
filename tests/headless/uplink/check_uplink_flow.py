#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='uplink-flow-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
        '-fno-pie','-no-pie','-I'+str(ROOT/'app/zhaoxi'),str(ROOT/'app/zhaoxi/uplink_flow.c'),
        str(ROOT/'tests/headless/uplink/test_uplink_flow.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

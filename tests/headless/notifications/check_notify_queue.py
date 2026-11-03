#!/usr/bin/env python3
"""Compile the real queue/parser/service against synthetic SDK boundaries."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='zhaoxi-notify-queue-') as tmp:
    for name in ('queue','service'):
        exe=Path(tmp)/name
        sources=[ROOT/'app/zhaoxi/notifications/notify_queue.c',ROOT/'app/zhaoxi/notifications/notify_intent.c']
        if name=='service':sources.append(ROOT/'app/zhaoxi/notifications/notify_service.c')
        command=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-pie','-no-pie','-g',
            '-I'+str(HERE/'include'),'-I'+str(ROOT/'app/zhaoxi/notifications'),'-I'+str(ROOT/'app/zhaoxi'),
            *map(str,sources),str(HERE/('notify_'+name+'_harness.c')),'-pthread','-o',str(exe)]
        subprocess.run(command,check=True)
        subprocess.run([str(exe)],check=True)

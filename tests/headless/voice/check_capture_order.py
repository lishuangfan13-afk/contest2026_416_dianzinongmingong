#!/usr/bin/env python3
"""Run actual SDK recording branches with explicit capture and ASR test doubles."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[3]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    args=parser.parse_args();sdk=args.sdk.resolve()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-capture-') as folder:
        temp=Path(folder);source=temp/'src/voice/voice_channel.c'
        source.parent.mkdir(parents=True)
        actual=(sdk/'src/voice/voice_channel.c').read_bytes();source.write_bytes(actual)
        # Sequential patch replay is covered by tools/check_agent_patches.py.
        inc=temp/'include';inc.mkdir()
        (inc/'agent_compat.h').write_text('''#pragma once
#include <stdbool.h>
#include <syslog.h>
#include <stddef.h>
#define OK 0
#define ERROR -1
int agent_task_create(void *(*f)(void *),const char *,int,void *,int);
''')
        (inc/'agent_config.h').write_text('''#pragma once
#define AGENT_ASR_CHUNK_SIZE 512
#define AGENT_AUDIO_CAPTURE_DEV "test_capture"
#define AGENT_AUDIO_PLAYBACK_DEV "test_playback"
#define AGENT_VOICE_SAMPLE_RATE 16000
#define AGENT_VOICE_CHANNELS 1
#define AGENT_VOICE_BITS 16
#define AGENT_VOICE_PCM_BUF_SIZE 65536
#define AGENT_VOICE_STACK 131072
#define AGENT_VOICE_PRIO 50
#define AGENT_CHAN_VOICE "voice"
#define AGENT_TTS_WS_SAMPLE_RATE 16000
''')
        exe=temp/'test_capture_order'
        subprocess.run(['gcc','-std=gnu11','-g','-O1','-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
                        '-Wl,--gc-sections','-pthread','-Wall','-Wextra',
                        '-I'+str(inc),'-I'+str(sdk/'include'),'-I'+str(sdk/'src'),
                        '-DVOICE_CHANNEL_SOURCE="'+str(source)+'"',
                        str(Path(__file__).with_name('test_capture_order.c')),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=15)
if __name__=='__main__':main()

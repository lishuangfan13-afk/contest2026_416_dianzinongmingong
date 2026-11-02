#!/usr/bin/env python3
"""Actual SDK autonomous voice state/endpoint test with synthetic PCM."""
from pathlib import Path
import argparse,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    args=parser.parse_args();sdk=args.sdk.resolve()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-auto-voice-') as folder:
        temp=Path(folder);source=temp/'src/voice/voice_channel.c';source.parent.mkdir(parents=True)
        names=['src/voice/voice_channel.c','src/voice/voice_channel.h',
               'src/voice/voice_wake.c','src/voice/voice_wake.h','CMakeLists.txt','Makefile']
        original={name:(sdk/name).read_bytes() for name in names}
        for name in names:(temp/name).write_bytes(original[name])
        # Full sequential SDK patch replay is checked by check_agent_patches.py.
        inc=temp/'include';inc.mkdir()
        (inc/'agent_compat.h').write_text('''#pragma once
#include <stdbool.h>
#include <syslog.h>
#include <stddef.h>
#define OK 0
#define ERROR -1
bool agent_shutdown_requested(void);
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
        exe=temp/'test_auto_voice'
        subprocess.run(['gcc','-std=gnu11','-g','-O1','-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections',
                        '-Wl,--gc-sections','-pthread','-Wall','-Wextra','-DCONFIG_ZHAOXI_BES_AUDIO=1',
                        '-I'+str(inc),'-I'+str(sdk/'include'),'-I'+str(sdk/'src'),
                        '-DVOICE_CHANNEL_SOURCE="'+str(source)+'"',
                        str(Path(__file__).with_name('test_auto_voice.c')),str(sdk/'src/voice/voice_wake.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=35)
if __name__=='__main__':main()

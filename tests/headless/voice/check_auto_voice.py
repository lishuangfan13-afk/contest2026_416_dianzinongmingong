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
        names=['src/voice/voice_channel.c','src/voice/voice_channel.h']
        original={name:(sdk/name).read_bytes() for name in names}
        for name in names:(temp/name).write_bytes(original[name])
        patches=[ROOT/'patches/0034-packages_ai_agent-autonomous-local-voice-terminal.patch',
                 ROOT/'patches/0036-packages_ai_agent-fix-automatic-turn-boundaries.patch']
        for patch in reversed(patches):
            subprocess.run(['patch','-R','-p1','--batch','-i',str(patch)],cwd=temp,check=True,capture_output=True)
        for patch in patches:
            subprocess.run(['patch','-p1','--batch','-i',str(patch)],cwd=temp,check=True,capture_output=True)
        assert all((temp/name).read_bytes()==original[name] for name in names)
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
                        '-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
                        '-Wl,--gc-sections','-pthread','-Wall','-Wextra','-DCONFIG_ZHAOXI_BES_AUDIO=1',
                        '-I'+str(inc),'-I'+str(sdk/'include'),'-I'+str(sdk/'src'),
                        '-DVOICE_CHANNEL_SOURCE="'+str(source)+'"',
                        str(Path(__file__).with_name('test_auto_voice.c')),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=35)
if __name__=='__main__':main()

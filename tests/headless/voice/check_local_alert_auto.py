#!/usr/bin/env python3
"""Run the actual reminder fallback with the actual automatic voice worker."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-local-output-') as folder:
        temp = Path(folder)
        inc = temp / 'include'
        inc.mkdir()
        (inc / 'agent_compat.h').write_text('''#pragma once
#include <stdbool.h>
#include <syslog.h>
#include <stddef.h>
#define OK 0
#define ERROR -1
bool agent_shutdown_requested(void);
int agent_task_create(void *(*f)(void *),const char *,int,void *,int);
''')
        (inc / 'agent_config.h').write_text('''#pragma once
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
        executable = temp / 'test_local_alert_auto'
        subprocess.run([
            'gcc', '-std=gnu11', '-g', '-O1', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-fno-sanitize-recover=all',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-pthread', '-Wall', '-Wextra', '-DCONFIG_ZHAOXI_BES_AUDIO=1',
            '-I' + str(inc), '-I' + str(sdk / 'include'), '-I' + str(sdk / 'src'),
            '-I' + str(ROOT / 'app/zhaoxi'),
            '-DVOICE_CHANNEL_SOURCE="' + str(sdk / 'src/voice/voice_channel.c') + '"',
            str(Path(__file__).with_name('test_local_alert_auto.c')),
            str(sdk / 'src/voice/voice_wake.c'), '-o', str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=35)

if __name__ == '__main__':
    main()

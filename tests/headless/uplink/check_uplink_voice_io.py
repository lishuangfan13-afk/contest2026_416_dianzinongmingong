#!/usr/bin/env python3
"""Actual offline voice IO with synthetic audio and a disposable fixed model path."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='uplink-voice-io-') as folder:
    p=Path(folder);inc=p/'include';inc.mkdir()
    headers={
      'agent_config.h':'#pragma once\n',
      'agent_compat.h':'#pragma once\n#include <stdbool.h>\nbool agent_shutdown_requested(void);\n',
      'core/message_bus.h':'#pragma once\n#include <stdbool.h>\nbool message_bus_is_ready(void);\n',
      'infra/task_lifecycle.h':'#pragma once\n#include <stdbool.h>\nbool agent_instance_is_active(void);\n',
      'voice/audio_capture.h':'''#pragma once
#include <stddef.h>
typedef struct audio_capture audio_capture_t;
audio_capture_t *audio_capture_open(const char *,unsigned,unsigned,unsigned);
int audio_capture_start(audio_capture_t *);
int audio_capture_read(audio_capture_t *,void *,size_t);
int audio_capture_abort(audio_capture_t *);
void audio_capture_close(audio_capture_t *);
''',
      'voice/audio_playback.h':'''#pragma once
#include <stddef.h>
typedef struct audio_playback audio_playback_t;
audio_playback_t *audio_playback_open(const char *,unsigned,unsigned,unsigned);
int audio_playback_write(audio_playback_t *,const void *,size_t);
void audio_playback_close(audio_playback_t *);
'''}
    for name,text in headers.items():
        target=inc/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
    exe=p/'test'
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-D_DEFAULT_SOURCE',
      '-DAGENT_DATA_DIR="'+str(p/'data')+'"','-O1','-g','-Wall','-Wextra','-Werror',
      '-fsanitize=address,undefined','-fno-pie','-no-pie','-fno-omit-frame-pointer',
      '-I'+str(inc),'-I'+str(ROOT/'app/zhaoxi'),
      str(ROOT/'tests/headless/uplink/test_uplink_voice_io.c'),str(ROOT/'app/zhaoxi/uplink_keywords.c'),
      '-Wl,--wrap=fsync','-Wl,--wrap=rename','-Wl,--wrap=clock_gettime','-pthread','-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)

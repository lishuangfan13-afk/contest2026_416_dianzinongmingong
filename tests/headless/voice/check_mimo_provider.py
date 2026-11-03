#!/usr/bin/env python3
"""Execute real MiMo request/recognize with real JSON/base64 and mocked HTTPS."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--workspace',type=Path,required=True);a=p.parse_args()
sdk=a.workspace/'packages/ai_agent';mbed=a.workspace/'apps/crypto/mbedtls/mbedtls';cjson=a.workspace/'apps/netutils/cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='zhaoxi-mimo-provider-') as folder:
    temp=Path(folder)
    (temp/'agent_compat.h').write_text('#pragma once\n#include <stdbool.h>\nbool agent_shutdown_requested(void);\n')
    (temp/'agent_config.h').write_text('#pragma once\n')
    exe=temp/'test'
    subprocess.run(['gcc','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-I'+str(temp),'-I'+str(ROOT/'app/zhaoxi'),'-I'+str(sdk/'src'),'-I'+str(sdk/'include'),'-I'+str(cjson),'-I'+str(mbed/'include'),
        '-DMIMO_SOURCE="'+str(ROOT/'app/zhaoxi/mimo_voice.c')+'"',str(Path(__file__).with_name('test_mimo_provider.c')),
        str(cjson/'cJSON.c'),str(mbed/'library/base64.c'),str(mbed/'library/constant_time.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)

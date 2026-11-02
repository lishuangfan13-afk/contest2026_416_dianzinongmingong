#!/usr/bin/env python3
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);p.add_argument('--cjson',type=Path,required=True);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='zhaoxi-morning-net-') as temp:
    inc=Path(temp)/'include';inc.mkdir()
    (inc/'agent_compat.h').write_text('''#pragma once
#include <stdbool.h>
#include <stddef.h>
bool agent_shutdown_requested(void);
int agent_secure_random(void *,size_t);
''')
    exe=Path(temp)/'test'
    subprocess.run(['gcc','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-pthread',
                    '-I'+str(inc),'-I'+str(ROOT/'tests/headless/include'),'-I'+str(ROOT/'app/zhaoxi'),
                    '-I'+str(a.sdk/'src'),'-I'+str(a.cjson),'-DAGENT_DATA_DIR="'+str(Path(temp)/'data')+'"',
                    str(ROOT/'tests/headless/test_morning_net.c'),str(ROOT/'app/zhaoxi/morning_store.c'),
                    str(ROOT/'app/zhaoxi/morning_net.c'),str(a.cjson/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=15)

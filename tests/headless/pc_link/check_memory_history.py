#!/usr/bin/env python3
"""Compile real memory, session and context implementations with actual cJSON."""
from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--cjson',type=Path,required=True);p.add_argument('--before-dir',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='zhaoxi-memory-') as temp:
 t=Path(temp);defs=[]
 for key,value in [('AGENT_DATA_DIR',str(t/'data')),('AGENT_CONFIG_DIR',str(t/'data/config')),('AGENT_SESSION_DIR',str(t/'data/sessions')),('AGENT_MEMORY_FILE',str(t/'data/memory/MEMORY.md')),('AGENT_SOUL_FILE',str(t/'data/config/SOUL.md')),('AGENT_USER_FILE',str(t/'data/config/USER.md')),('AGENT_TIMEZONE','CST-8')]:defs+=['-D'+key+'="'+value+'"']
 sources=[str(a.before_dir/('before53-'+name)) if a.before_dir else str(a.sdk/'src/core'/name) for name in ['memory_store.c','session_mgr.c','context_builder.c']]
 if a.before_dir:defs+=['-DEXPECT_BROKEN=1']
 command=['gcc','-std=gnu11','-D_GNU_SOURCE','-D_POSIX_C_SOURCE=200809L','-DAGENT_SESSION_MAX_MSGS=20',*defs,'-Wall','-Wextra','-Werror','-Wno-format-truncation','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-include','errno.h','-include','pthread.h','-include','stdlib.h','-I'+str(r/'tests/headless/include'),'-I'+str(a.sdk/'src'),'-I'+str(a.sdk/'include'),'-I'+str(a.cjson),str(Path(__file__).with_name('test_memory_history.c')),*sources,str(a.cjson/'cJSON.c'),'-pthread','-lm','-Wl,--wrap=time','-Wl,--wrap=fopen','-o',str(t/'test')]
 subprocess.run(command,check=True);subprocess.run([str(t/'test')],check=True,timeout=20)

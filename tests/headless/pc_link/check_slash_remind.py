#!/usr/bin/env python3
"""Exercise actual slash reminder and cron tool/persistence, with old-source repro."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path)
p.add_argument('--before-dir',type=Path)
a=p.parse_args();r=Path(__file__).resolve().parents[3]
cjson=a.sdk.parents[1]/'apps/netutils/cjson/cJSON'
loop=(a.before_dir/'before60-agent_loop.c' if a.before_dir else a.sdk/'src/core/agent_loop.c').read_text()
start=loop.index('static char* handle_slash_remind(const agent_msg_t* msg)\n{')
actual=loop[start:loop.index('/* ── Extracted: handle vision message',start)]
template=Path(__file__).with_name('test_slash_remind.c').read_text().replace('/* ACTUAL_SLASH_REMIND */',actual)
with tempfile.TemporaryDirectory(prefix='zhaoxi-slash-remind-') as d:
    d=Path(d);source=d/'test.c';source.write_text(template)
    tool=a.before_dir/'before60-tool_cron.c' if a.before_dir else a.sdk/'src/tools/tool_cron.c'
    profiles=[('old',['-DEXPECT_BROKEN=1','-DCONFIG_EXAMPLES_ZHAOXI=1','-DCONFIG_ZHAOXI_BES_AUDIO=1'])] if a.before_dir else [
        ('product',['-DCONFIG_EXAMPLES_ZHAOXI=1','-DCONFIG_ZHAOXI_BES_AUDIO=1']),('standard',[])]
    for name,profile in profiles:
        exe=d/name
        legacy_warnings=['-Wno-error=format-truncation'] if a.before_dir else []
        command=['cc','-std=gnu11','-D_GNU_SOURCE','-D_POSIX_C_SOURCE=200809L',*profile,
            '-DAGENT_DATA_DIR="'+str(d/(name+'_data'))+'"','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',*legacy_warnings,
            '-include',str(r/'tests/headless/cron_support.h'),
            '-I'+str(r/'tests/headless/include'),'-I'+str(a.sdk/'src'),'-I'+str(cjson),
            str(source),str(tool),str(a.sdk/'src/infra/cron_service.c'),str(a.sdk/'src/core/message_bus.c'),
            str(cjson/'cJSON.c'),'-Wl,--wrap=time','-pthread','-lm','-o',str(exe)]
        subprocess.run(command,check=True)
        subprocess.run([str(exe)],check=True,timeout=20)

#!/usr/bin/env python3
"""Run actual ReAct and message constructors with actual SDK file tools."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path)
p.add_argument('--cjson',required=True,type=Path)
p.add_argument('--loop-source',type=Path)
p.add_argument('--expect-broken',action='store_true')
p.add_argument('--standard',action='store_true')
p.add_argument('--files-source',type=Path)
p.add_argument('--expect-edit-broken',action='store_true')
p.add_argument('--expect-cron-broken',action='store_true')
a=p.parse_args()
root=Path(__file__).resolve().parents[3]
text=(a.loop_source or a.sdk/'src/core/agent_loop.c').read_text()
template=Path(__file__).with_name('test_mutation_guard.c').read_text()
assistant=text[text.index('static void add_assistant_message('):text.index('/* A transport turn ID')]
tools=text[text.index('/* Parallel tool execution context */'):text.index('/* ── Fast path counters')]
helpers='' if a.expect_broken else text[text.index('#ifdef CONFIG_EXAMPLES_ZHAOXI\n/* Side-effect confirmation'):text.index('/* Run the ReAct tool-calling loop.')]
loop=text[text.index('static char* run_react_loop('):text.index('/* ── Build messages array')]
code=template.replace('/* ACTUAL_ASSISTANT_MESSAGES */',assistant).replace('/* ACTUAL_TOOL_MESSAGES */',tools).replace('/* ACTUAL_MUTATION_HELPERS */',helpers).replace('/* ACTUAL_REACT_LOOP */',loop)
with tempfile.TemporaryDirectory(prefix='zhaoxi-mutation-') as folder:
    folder=Path(folder);source=folder/'test.c';source.write_text(code)
    flags=['-DEXPECT_BROKEN=1'] if a.expect_broken else ['-DEXPECT_EDIT_BROKEN=1'] if a.expect_edit_broken else ['-DEXPECT_CRON_BROKEN=1'] if a.expect_cron_broken else []
    if 'PRODUCT_MUTATION_CRON_ADD' in text and not (a.standard or a.expect_broken or a.expect_edit_broken or a.expect_cron_broken): flags+=['-DCHECK_CRON_GUARD=1']
    profile=[] if a.standard else ['-DCONFIG_EXAMPLES_ZHAOXI=1']
    command=['gcc','-std=gnu11','-D_GNU_SOURCE','-D_POSIX_C_SOURCE=200809L',*profile,*flags,
        '-DAGENT_MAX_TOOL_CALLS=4','-DAGENT_CONFIG_FILE=AGENT_DATA_DIR"/config.json"',
        '-DAGENT_SOUL_FILE=AGENT_DATA_DIR"/SOUL.md"','-DAGENT_USER_FILE=AGENT_DATA_DIR"/USER.md"',
        '-DAGENT_DATA_DIR="'+str(folder/'data')+'"','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-include','unistd.h','-include',str(root/'tests/headless/cron_support.h'),
        '-I'+str(root/'tests/headless/include'),'-I'+str(a.sdk/'src'),'-I'+str(a.sdk/'include'),'-I'+str(a.cjson),
        str(source),str(a.files_source or a.sdk/'src/tools/tool_files.c'),str(a.sdk/'src/tools/tool_cron.c'),str(a.sdk/'src/infra/cron_service.c'),str(a.cjson/'cJSON.c'),'-pthread','-lm','-Wl,--wrap=fopen','-o',str(folder/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(folder/'test')],check=True,timeout=25)

#!/usr/bin/env python3
"""Exercise real Agent ownership/network-watcher code without board or UART."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',required=True,type=Path);p.add_argument('--cjson',required=True,type=Path);p.add_argument('--pc-link-source',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[3]
text=(a.sdk/'src/agent_main.c').read_text()
globals=text[text.index('static volatile bool g_shutdown_requested'):text.index('/* ── Network watcher')]
pause=text[text.index('static bool headless_network_pause('):text.index('#endif',text.index('static bool headless_network_pause('))]
watcher=text[text.index('static void* network_watch_task('):text.index('#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW',text.index('static void* network_watch_task('))]
entry=text[text.index('int ai_agent_main('):]
link=(a.pc_link_source or r/'app/zhaoxi/pc_link.c').read_text()
stop=link[link.index('static int stop_session_agent('):link.index('int zhaoxi_pc_link(')]
assert link.index('stop_session_agent(session_owner, 45000, report)')<link.index('atomic_store(&stopping, true);\n    pthread_join(worker, NULL);')
assert 'agent_instance_is_active()' in link and 'ai_agent --headless --pc-owner %llu &' in link
template=Path(__file__).with_name('test_owned_shutdown.c').read_text()
code=template.replace('/* ACTUAL_SHUTDOWN_GLOBALS */',globals).replace('/* ACTUAL_NETWORK_PAUSE */',pause).replace('/* ACTUAL_NETWORK_WATCHER */',watcher).replace('/* ACTUAL_INSTANCE_ENTRY */',entry).replace('/* ACTUAL_SESSION_STOP */',stop)
with tempfile.TemporaryDirectory(prefix='zhaoxi-owned-stop-') as folder:
    folder=Path(folder);source=folder/'test.c';source.write_text(code);exe=folder/'test'
    subprocess.run(['gcc','-std=gnu11','-D_GNU_SOURCE','-D_POSIX_C_SOURCE=200809L','-DCONFIG_EXAMPLES_ZHAOXI=1',
        '-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-I'+str(a.sdk/'src'),'-I'+str(a.cjson),str(source),str(a.sdk/'src/infra/task_lifecycle.c'),
        str(a.cjson/'cJSON.c'),'-pthread','-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)

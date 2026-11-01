#!/usr/bin/env python3
"""Compile actual product parser/storage and actual Agent worker/dispatch with explicit service doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk', required=True, type=Path)
a = p.parse_args()
r = Path(__file__).resolve().parents[2]
cjson = a.sdk.parents[1] / 'apps/netutils/cjson/cJSON'
s = (a.sdk / 'src/core/agent_loop.c').read_text()
pieces = {
    '/* ACTUAL_ELAPSED */': s[s.index('static inline uint32_t calc_elapsed_ms('):s.index('/* ── Memory pool')],
    '/* ACTUAL_VOICE_REPLY */': s[s.index('static bool automatic_voice_message('):s.index('/* Auto-inject channel')],
    '/* ACTUAL_DISPATCH_AND_HOOK */': s[s.index('static void dispatch_response('):s.index('/* ── ReAct tool-calling loop')],
    '/* ACTUAL_AGENT_WORKER */': s[s.index('static void* agent_loop_task('):s.index('/* ── Public interface')],
}
assert s.index('if (!reply && tool_guard_check_injection') < s.index('if (!reply && try_product_local_reply') < s.index('reply = handle_nl_fast_path(')
template = (r / 'tests/headless/test_local_intent.c').read_text()
for key, code in pieces.items():
    template = template.replace(key, code)
with tempfile.TemporaryDirectory(prefix='zhaoxi-local-intent-') as d:
    d = Path(d)
    source = d / 'test.c'; source.write_text(template)
    flags = ['-std=gnu11', '-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200809L',
             '-DCONFIG_EXAMPLES_ZHAOXI=1', '-DCONFIG_ZHAOXI_BES_AUDIO=1', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
             '-I'+str(r/'tests/headless/include'), '-I'+str(r/'app/zhaoxi'),
             '-I'+str(a.sdk/'src'), '-I'+str(a.sdk/'include'), '-I'+str(cjson),
             '-include',str(r/'tests/headless/cron_support.h'), '-DAGENT_DATA_DIR="'+str(d/'data')+'"']
    local_obj = d / 'local.o'
    subprocess.run(['cc', *flags, '-c', str(r/'app/zhaoxi/local_intent.c'), '-o', str(local_obj)], check=True)
    archive = d / 'libproduct.a'
    subprocess.run(['ar', 'rcs', str(archive), str(local_obj)], check=True)
    for name, extra, local in [('product', [], [str(archive), str(r/'app/zhaoxi/journal.c'), str(r/'app/zhaoxi/todo_store.c'),
                                               str(r/'app/zhaoxi/reminder.c'),str(r/'app/zhaoxi/drink.c'),str(a.sdk/'src/infra/cron_service.c')]),
                               ('without_hooks', ['-DTEST_NO_HOOKS=1'], [])]:
        exe = d / name
        subprocess.run(['cc', *flags, *extra, str(source), *local,
                        str(a.sdk/'src/core/message_bus.c'), str(cjson/'cJSON.c'),
                        '-Wl,--wrap=time', '-Wl,--wrap=fsync', '-pthread', '-lm', '-o', str(exe)], check=True)
        if name == 'product':
            symbols = subprocess.check_output(['nm', '-g', str(exe)], text=True)
            for symbol in ('agent_product_local_match', 'agent_product_local_execute'):
                assert any(line.endswith(' T '+symbol) for line in symbols.splitlines()), symbols
            print('PASS: explicit main anchor pulls both strong product hooks from static archive')
        subprocess.run([str(exe)], check=True, timeout=20)

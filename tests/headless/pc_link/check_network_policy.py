#!/usr/bin/env python3
"""Run the actual network watcher in real threads for PC/Wi-Fi and standard profiles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk', required=True, type=Path)
a = p.parse_args()
source = (a.sdk / 'src/agent_main.c').read_text()
globals_ = source[source.index('static volatile bool g_shutdown_requested'):
                  source.index('/* ── Network watcher')]
start = source.index('static bool headless_network_pause(')
product = source[start:source.index('#endif', start)]
start = source.index('static void* network_watch_task(')
watcher = source[start:source.index('#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW', start)]
template = Path(__file__).with_name('test_network_policy.c').read_text()
code = template.replace('/* ACTUAL_SHUTDOWN_GLOBALS */', globals_).replace(
    '/* ACTUAL_PRODUCT_HELPERS */', product).replace('/* ACTUAL_NETWORK_WATCHER */', watcher)
with tempfile.TemporaryDirectory(prefix='zhaoxi-uplink-policy-') as directory:
    directory = Path(directory)
    test = directory / 'test.c'
    test.write_text(code)
    for name, flags in [('product', ['-DCONFIG_EXAMPLES_ZHAOXI=1']), ('standard', [])]:
        exe = directory / name
        subprocess.run(['gcc', '-std=gnu11', '-D_GNU_SOURCE', *flags,
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                        str(test), '-pthread', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=15)

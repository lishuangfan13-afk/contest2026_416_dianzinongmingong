#!/usr/bin/env python3
"""Actual background TLS BIO deadline/cancel helpers on real stalled sockets."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path)
a=p.parse_args()
source=(a.sdk/'src/infra/vela_tls.c').read_text()
actual=source[source.index('static int64_t bounded_now_ms('):source.index('static int tls_ctx_connect_limited(')]
template=Path(__file__).with_name('test_morning_tls.c').read_text()
with tempfile.TemporaryDirectory(prefix='zhaoxi-morning-tls-') as folder:
    root=Path(folder);test=root/'test.c';binary=root/'test'
    test.write_text(template.replace('/* ACTUAL_BOUNDED_BIO */',actual))
    subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    str(test),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=8)

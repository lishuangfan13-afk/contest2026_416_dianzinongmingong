#!/usr/bin/env python3
"""Exercise actual HTTP writer and header reader with explicit TLS/IO doubles."""
from pathlib import Path
import argparse, subprocess, tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',type=Path,required=True)
a=p.parse_args()
text=(a.sdk/'src/infra/vela_tls.c').read_text()
begin=text.index('#ifdef CONFIG_ZHAOXI_BES_AUDIO\n/* Counters describe plaintext')
end=text.index('\n/**\n * Read full HTTP/1.1 response.',begin)
reader_begin=text.index('static int tls_read_response(',end)
reader_end=text.index('/* ── Bounded incremental HTTP response delivery',reader_begin)
actual=text[begin:end]+text[reader_begin:reader_end]
template=Path(__file__).with_name('test_tls_write_diagnostics.c').read_text()
with tempfile.TemporaryDirectory(prefix='zhaoxi-tls-write-diag-') as folder:
    folder=Path(folder);source=folder/'test.c';exe=folder/'test'
    source.write_text(template.replace('/* ACTUAL_TLS_WRITER_READER */',actual))
    for product in (False,True):
        flags=['-DCONFIG_ZHAOXI_BES_AUDIO=1','-D__NuttX__=1'] if product else []
        subprocess.run(['gcc','-D_GNU_SOURCE',*flags,'-std=gnu11','-O1','-g',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror',
            '-I'+str(a.sdk/'src'),str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=20)

#!/usr/bin/env python3
"""Exercise the actual incremental HTTPS parser with fragmented TLS test doubles."""
from pathlib import Path
import argparse, subprocess, tempfile

ROOT=Path(__file__).resolve().parents[3]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',required=True,type=Path)
    a=p.parse_args();sdk=a.sdk.resolve()
    actual=(sdk/'src/infra/vela_tls.c').read_text()
    begin=actual.index('/* ── Bounded incremental HTTP response delivery')
    end=actual.index('typedef struct {\n    char *buffer;\n    size_t capacity, used;',begin)
    public_begin=actual.index('int vela_https_post_json_stream(')
    public_end=actual.index('int vela_https_head_date(',public_begin)
    with tempfile.TemporaryDirectory(prefix='zhaoxi-tls-stream-') as folder:
        folder=Path(folder)
        source=folder/'test.c';exe=folder/'test'
        template=Path(__file__).with_name('test_tls_stream.c').read_text()
        source.write_text(template.replace('/* ACTUAL_STREAM_IMPLEMENTATION */',actual[begin:end]+actual[public_begin:public_end]))
        for product in (False, True):
            flags=['-DCONFIG_ZHAOXI_BES_AUDIO=1'] if product else []
            subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer','-Wall','-Wextra','-Werror','-pthread',
                            *flags,'-I'+str(sdk/'src'),str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=20)

if __name__=='__main__':main()

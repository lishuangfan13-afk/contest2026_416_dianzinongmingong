#!/usr/bin/env python3
"""Compile the full actual TLS translation unit; substitute only TLS reads."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path)
p.add_argument('--mbedtls',required=True,type=Path,help='actual mbedtls include directory')
p.add_argument('--source',type=Path,help='optional whole source snapshot for an old-bug reproduction')
p.add_argument('--expect-broken',action='store_true')
a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='zhaoxi-tls-framing-') as folder:
    folder=Path(folder);inc=folder/'include';inc.mkdir()
    (inc/'agent_compat.h').write_text('#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#include <syslog.h>\nint agent_secure_random(void *,size_t);\n')
    (inc/'agent_config.h').write_text('#pragma once\n#define AGENT_TLS_CONN_POOL_SIZE 2\n#define AGENT_LLM_SOCKET_TIMEOUT_SEC 35\n')
    for product in (False,True):
        exe=folder/('product' if product else 'standard')
        flags=['-DCONFIG_ZHAOXI_BES_AUDIO=1','-DCONFIG_EXAMPLES_ZHAOXI=1'] if product else []
        if a.source:flags+=['-DOLD_TLS_RAW_SIGNATURE=1']
        subprocess.run(['gcc','-D_GNU_SOURCE','-std=gnu11','-g','-O1','-Wall','-Wextra',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread',*flags,
            '-I'+str(inc),'-I'+str(a.sdk/'src'),'-I'+str(a.mbedtls),
            '-DVELA_TLS_SOURCE="'+str(a.source or a.sdk/'src/infra/vela_tls.c')+'"',
            str(Path(__file__).with_name('test_tls_buffered_framing.c')),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=25)
        if a.expect_broken:
            if not result.returncode or 'Assertion' not in result.stderr:
                raise RuntimeError('Old framing defect not reproduced: '+result.stdout+result.stderr)
            print('REPRO: whole old production TLS source fails valid extension/error-body assertions as expected')
        elif result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
        else:
            print(result.stdout,end='')

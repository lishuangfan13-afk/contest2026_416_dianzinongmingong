#!/usr/bin/env python3
"""Exercise actual lifecycle and actual NuttX TLS pop ordering, including old UAF repro."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path)
p.add_argument('--before-source',type=Path)
p.add_argument('--evidence-dir',type=Path)
a=p.parse_args();r=Path(__file__).resolve().parents[3]
ws=a.sdk.parents[1]
nuttx=(ws/'nuttx/libs/libc/tls/tls_cleanup.c').read_text()
nuttx=nuttx[nuttx.index('void tls_cleanup_push('):]
template=Path(__file__).with_name('test_cleanup_tokens.c').read_text().replace('/* ACTUAL_NUTTX_TLS_CLEANUP */',nuttx)
source=a.before_source or a.sdk/'src/infra/task_lifecycle.c'
with tempfile.TemporaryDirectory(prefix='zhaoxi-cleanup-token-') as d:
    d=Path(d);test=d/'test.c';test.write_text(template)
    both=['-fsanitize=address,undefined']
    cases=[('old_nuttx',['-DTEST_NUTTX_STYLE=1','-DEXPECT_BROKEN=1'],both)] if a.before_source else [
        ('actual_nuttx_order',['-DTEST_NUTTX_STYLE=1'],both),
        ('native_pthread_asan',['-DTEST_SKIP_NATIVE_UNWIND=1'],both),
        # This host libasan fails its own stack-magic CHECK on glibc forced
        # unwind. Real pthread_exit/cancel are tested with UBSan instead;
        # NuttX-order normal and reentrant cleanup above retains ASan too.
        ('native_pthread_exit_cancel',[],['-fsanitize=undefined'])]
    for name,flags,sanitizers in cases:
        exe=d/name
        subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-D_POSIX_C_SOURCE=200809L',*flags,
            '-DLIFECYCLE_SOURCE="'+str(source)+'"','-Wall','-Wextra','-Werror',
            *sanitizers,'-fno-sanitize-recover=all','-fno-omit-frame-pointer',
            '-I'+str(a.sdk/'src'),'-I'+str(a.sdk/'src/infra'),str(test),'-Wl,--wrap=free','-Wl,--wrap=pthread_create','-Wl,--wrap=pthread_cond_broadcast','-Wl,--wrap=pthread_setcancelstate',
            '-pthread','-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=25)
        if a.evidence_dir:
            a.evidence_dir.mkdir(parents=True,exist_ok=True)
            (a.evidence_dir/(name+'.log')).write_text(result.stdout+result.stderr)
        if a.before_source:
            assert result.returncode and 'heap-use-after-free' in result.stderr,result.stdout+result.stderr
            print('REPRO: actual old task_finished reads freed worker metadata when NuttX pop callback is re-entered before TLS record removal')
        else:
            if result.returncode:print(result.stdout,result.stderr)
            assert result.returncode==0
            print(result.stdout.strip())

#!/usr/bin/env python3
"""Exercise patched upstream PPP lifecycle on Linux, with explicit OS doubles."""
from pathlib import Path
import argparse, shutil, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps',type=Path,required=True,help='Pinned NuttX apps checkout')
    args=parser.parse_args()
    apps=args.apps.resolve()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-ppp-') as folder:
        out=Path(folder); source=out/'netutils/pppd';source.mkdir(parents=True)
        for name in ['pppd.c','ppp.h','ppp_conf.h','ppp_arch.h','ahdlc.h','ahdlc.c','lcp.h','ipcp.h','pap.h']:
            upstream=subprocess.check_output(['git','-C',str(apps),'show','HEAD:netutils/pppd/'+name])
            (source/name).write_bytes(upstream)
        subprocess.run(['patch','-p1','--batch','-i',str(ROOT/'patches/0025-apps-pppd-bounded-pc-link-lifecycle.patch')],cwd=out,check=True,capture_output=True)
        for name in ['pppd.c','ppp.h','ahdlc.c']:
            if (apps/'netutils/pppd'/name).read_bytes() != (source/name).read_bytes():
                raise RuntimeError('Actual workspace '+name+' does not equal pristine plus patch25')
        stubs=out/'include'
        for name,text in {
            'nuttx/config.h':'#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#define FAR\n#define CONFIG_EXAMPLES_ZHAOXI 1\nsize_t strlcpy(char *, const char *, size_t);\n',
            'nuttx/net/tun.h':'#pragma once\n#define TUNSETIFF 0x400454ca\n#define IFF_TUN 1\n',
            'debug.h':'#pragma once\n#define ninfo(...) ((void)0)\n',
            'netutils/netlib.h':'#pragma once\nint netlib_ifdown(const char *);\n',
        }.items():
            p=stubs/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
        for name in ['chat.h','pppd.h']:
            shutil.copy2(apps/'include/netutils'/name,stubs/'netutils'/name)
        exe=out/'test_lifecycle'
        subprocess.run(['gcc','-std=gnu11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-Wall','-Wextra','-Wno-unused-parameter','-I'+str(stubs),'-I'+str(source),
                        str(ROOT/'tests/headless/ppp/test_lifecycle.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        burst=out/'test_ahdlc_burst'
        subprocess.run(['gcc','-std=gnu11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-Wall','-Wextra','-Wno-unused-parameter','-I'+str(stubs),'-I'+str(source),
                        str(ROOT/'tests/headless/ppp/test_ahdlc_burst.c'),'-o',str(burst)],check=True)
        subprocess.run([str(burst)],check=True)
if __name__=='__main__':main()

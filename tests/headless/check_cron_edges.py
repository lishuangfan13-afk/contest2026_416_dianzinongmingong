#!/usr/bin/env python3
"""Exercise actual cron implementation with controlled wall clock and delivery."""
from pathlib import Path
import argparse, subprocess, tempfile

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk',required=True,type=Path)
parser.add_argument('--cjson',required=True,type=Path)
args=parser.parse_args()
for product in (False,True):
    with tempfile.TemporaryDirectory(prefix='zhaoxi-cron-edges-') as folder:
        path=Path(folder)
        definitions=['-DCONFIG_EXAMPLES_ZHAOXI=1','-DCONFIG_ZHAOXI_BES_AUDIO=1'] if product else []
        executable=path/'test_cron_edges'
        subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L',*definitions,
            '-DAGENT_DATA_DIR="'+str(path/'data')+'"','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined,float-cast-overflow','-fno-omit-frame-pointer',
            '-fno-sanitize-recover=all',
            '-I'+str(ROOT/'tests/headless/include'),'-I'+str(args.sdk/'src'),'-I'+str(args.sdk/'include'),
            '-I'+str(args.cjson),'-include',str(ROOT/'tests/headless/cron_support.h'),
            str(Path(__file__).with_name('test_cron_edges.c')),str(args.cjson/'cJSON.c'),
            '-pthread','-lm','-Wl,--wrap=time','-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True,timeout=20)

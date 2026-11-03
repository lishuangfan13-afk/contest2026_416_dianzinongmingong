#!/usr/bin/env python3
"""Execute actual product Wi-Fi readiness/power policy against ioctl doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);a=p.parse_args()
source=(a.sdk/'src/infra/network_manager.c').read_text()
policy=source[source.index('/* ZHAOXI_WIFI_AWAKE_BEGIN */'):source.index('/* ZHAOXI_WIFI_AWAKE_END */')]
ready=source[source.index('bool network_is_connected(void)'):source.index('\nint network_wait_connected(')]
with tempfile.TemporaryDirectory(prefix='zhaoxi-wifi-awake-') as folder:
    temp=Path(folder);(temp/'wifi_awake_actual.c').write_text(policy+ready)
    exe=temp/'test'
    subprocess.run(['gcc','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror','-pthread',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-I'+str(temp),str(Path(__file__).with_name('test_wifi_awake.c')),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

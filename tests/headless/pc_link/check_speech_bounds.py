#!/usr/bin/env python3
"""Execute actual bounded JSON/SSE entry points and optional Wi-Fi queue policy."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True);a=p.parse_args()
source=(a.sdk/'src/infra/vela_tls.c').read_text()
post=source[source.index('int vela_https_post_json_bounded('):source.index('\nint vela_https_get(',source.index('int vela_https_post_json_bounded('))]
stream=source[source.index('static int https_post_json_stream_impl('):source.index('\nint vela_https_head_date(',source.index('static int https_post_json_stream_impl('))]
get=source[source.index('int vela_https_get_bounded('):source.index('\nint vela_https_post_json(',source.index('int vela_https_get_bounded('))]
wifi=source[source.index('static int zhaoxi_apply_wifi_tx_buffer('):source.index('/* ZHAOXI_PPP_RX_WINDOW_END */')]
template=Path(__file__).with_name('test_speech_bounds.c').read_text()
with tempfile.TemporaryDirectory(prefix='zhaoxi-speech-bounds-') as temp:
    path=Path(temp);test=path/'test.c';exe=path/'test'
    test.write_text(template.replace('/* ACTUAL_BOUNDED_APIS */',post+get+stream).replace('/* ACTUAL_WIFI_POLICY */',wifi))
    subprocess.run(['gcc','-D_GNU_SOURCE','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I'+str(a.sdk/'src'),str(test),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

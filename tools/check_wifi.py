#!/usr/bin/env python3
"""Exercise actual SDK station-readiness/TCP functions against controlled POSIX boundaries.

The fixtures replace OS results. Passing them is not a live Wi-Fi or cloud test.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root',type=Path,required=True)
    parser.add_argument('--cc',default='cc')
    args=parser.parse_args()
    out=ROOT/'out/headless/wifi';out.mkdir(parents=True,exist_ok=True)
    network=(args.agent_root/'src/infra/network_manager.c').read_text()
    readiness=network[network.index('bool network_is_connected(void)'):network.index('int network_wait_connected(')]
    (out/'station_readiness.c').write_text(readiness)
    tls=(args.agent_root/'src/infra/vela_tls.c').read_text()
    at=tls.index('static int zhaoxi_tcp_connect(')
    tcp=tls[at:tls.index('\n#endif',at)]
    (out/'tcp_connect_helper.c').write_text(tcp)
    report={'board_operations':False,'tests':{}}
    for name in ('test_network_readiness','test_tcp_connect'):
        source=ROOT/'tests/headless/wifi'/(name+'.c')
        executable=out/name
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(out),
                        str(source),'-o',str(executable)],check=True)
        result=subprocess.run([str(executable)],check=True,capture_output=True,text=True)
        report['tests'][name]=result.stdout.strip()
    (out/'checks.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':
    main()

#!/usr/bin/env python3
"""Send a bounded command to the BES over its active PPP link; no serial access."""
import argparse,base64,json,os,socket,subprocess,sys,time
from pathlib import Path

def wsl_path(path):
    text=str(Path(path).resolve())
    return '/mnt/'+text[0].lower()+'/'+text[3:].replace('\\','/')

def process_error(stderr,returncode):
    """Show the useful child error, not WSL's mixed UTF-16 warning prefix."""
    traceback=stderr.find(b'Traceback (most recent call last):')
    if traceback>=0:
        text=stderr[traceback:].decode('utf-8',errors='replace')
    elif stderr.startswith(b'w\x00s\x00l\x00:'):
        # WSL may prepend its own UTF-16LE localhost/proxy warning to the
        # Python child's UTF-8 stderr. Keep an actual child error if present.
        end=stderr.find(b'\n\x00')
        remaining=stderr[end+2:] if end>=0 else b''
        remaining=remaining.lstrip(b'\x00\r\n')
        text=remaining.decode('utf-8',errors='replace') if remaining else ''
    elif b'\x00' in stderr:
        text=stderr.decode('utf-16-le',errors='replace')
    else:
        text=stderr.decode('utf-8',errors='replace')
    lines=[line.strip() for line in text.splitlines() if line.strip()]
    return lines[-1] if lines else 'Board command failed (WSL exit '+str(returncode)+')'

def request(value,timeout=90):
    if not 1<=timeout<=90:raise ValueError('Response timeout must be 1..90 seconds')
    encoded=base64.b64encode(json.dumps(value,ensure_ascii=False).encode()).decode()
    if os.name=='nt':
        p=subprocess.run(['wsl','-d','Ubuntu-22.04','--','python3',wsl_path(__file__),'--encoded',encoded,'--timeout',str(timeout)],
                         stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout+15,
                         creationflags=subprocess.CREATE_NO_WINDOW)
        # A board/tool business error is valid JSON and intentionally makes
        # the CLI exit nonzero. Preserve that response for API callers.
        try:
            result=json.loads(p.stdout)
        except (ValueError,UnicodeError):
            raise RuntimeError(process_error(p.stderr,p.returncode)) from None
        if not isinstance(result,dict) or 'rc' not in result:
            raise RuntimeError('Board command returned an invalid response')
        return result
    wire=json.dumps(value,ensure_ascii=False).encode()+b'\n'
    if len(wire)>2048:raise ValueError('Command too long')
    with socket.create_connection(('10.254.42.2',8765),min(10,timeout)) as peer:
        peer.settimeout(timeout);peer.sendall(wire);response=bytearray()
        while b'\n' not in response and len(response)<65536:
            data=peer.recv(4096)
            if not data:break
            response.extend(data)
    return json.loads(response)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    group=parser.add_mutually_exclusive_group(required=True)
    for name in ['status','quit','notes','tasks','reminders','time']:group.add_argument('--'+name,action='store_true')
    group.add_argument('--sync-time',action='store_true',help='Synchronize the board from this computer UTC time')
    group.add_argument('--profile',action='store_true',help='Read persistent user persona')
    group.add_argument('--set-profile',help='Replace user persona with verified persistent storage')
    group.add_argument('--news',action='store_true',help='Read morning service state')
    group.add_argument('--news-get',action='store_true',help='Read today cached briefing')
    group.add_argument('--auto-status' ,action='store_true',help='Read automatic voice and wake state without starting the microphone')
    group.add_argument('--cmd',nargs=argparse.REMAINDER)
    group.add_argument('--encoded',help=argparse.SUPPRESS)
    parser.add_argument('--timeout',type=int,default=90,help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.encoded:value=json.loads(base64.b64decode(args.encoded))
    elif args.cmd is not None:value={'action':'command','args':args.cmd}
    elif args.auto_status:value={'action':'auto-status'}
    elif args.sync_time:value={'action':'sync-time','epoch':int(time.time())}
    elif args.profile:value={'action':'profile-get'}
    elif args.set_profile is not None:value={'action':'profile-set','text':args.set_profile}
    elif args.news:value={'action':'news-status'}
    elif args.news_get:value={'action':'news-get'}
    else:
        action=next(name for name in ['status','quit','notes','tasks','reminders','time'] if getattr(args,name))
        value={'action':'ping' if action=='status' else action}
    try:result=request(value,args.timeout)
    except (OSError,ValueError,RuntimeError,subprocess.TimeoutExpired) as error:
        print('Board command failed: '+str(error),file=sys.stderr)
        return 2
    print(json.dumps(result,ensure_ascii=True,indent=2))
    return 0 if result.get('rc')==0 else 1

if __name__=='__main__':raise SystemExit(main())

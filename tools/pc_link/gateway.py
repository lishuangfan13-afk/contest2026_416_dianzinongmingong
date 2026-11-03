"""Private, authenticated loopback transport for a native PPP proof on WSL."""
from pathlib import Path
import argparse,hmac,json,os,select,signal,socket,subprocess,sys,time

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from morning_feed import FeedCompanion

def session_deadline(config,now=None):
    """Only explicit board-owned boot configuration can remove the deadline.

    Seconds-only legacy files retain the original seconds + 300s allowance,
    including finite behavior for zero. A standalone persistent flag is an
    error, not an escape hatch for ordinary PPP proof sessions.
    """
    now=time.monotonic() if now is None else now
    if config.get('persistent') is True:
        if config.get('boot_attach') is not True or type(config.get('seconds')) is not int or config['seconds']!=0:
            raise ValueError('Persistent PPP requires explicit boot_attach=true and seconds=0')
        return None
    return now+config['seconds']+300


def gateway_should_run(deadline,process,now=None):
    now=time.monotonic() if now is None else now
    return (deadline is None or now<deadline) and process.poll() is None


def run_gateway(config_path):
    config=json.loads(Path(config_path).read_text())
    # Validate persistent policy before network or process side effects.
    session_deadline(config,0)
    root=Path(config_path).parent
    pidfile=root/'pc_ppp_server.pid'
    def terminated(signum,frame): raise SystemExit(128+signum)
    signal.signal(signal.SIGTERM,terminated)
    rules=[];forward=Path('/proc/sys/net/ipv4/ip_forward');previous=forward.read_text().strip()
    server=socket.socket();server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    peer=None;proc=None;changed=False;logfile=None
    news_feed=FeedCompanion()
    def rule(table,*items):
        command=['iptables']+(['-t',table] if table else [])+['-I']+list(items)
        subprocess.run(command,check=True,capture_output=True)
        rules.append(command)
    try:
        server.bind(('127.0.0.1',config['port']));server.listen(1);server.settimeout(120)
        pidfile.write_text(str(os.getpid()))
        print('PPP_SERVER_READY',flush=True)
        peer,_=server.accept();peer.settimeout(5)
        auth=bytearray()
        while b'\n' not in auth and len(auth)<256:
            piece=peer.recv(1)
            if not piece:raise EOFError('Authentication transport closed')
            auth.extend(piece)
        if not hmac.compare_digest(bytes(auth).strip(),config['token'].encode()):raise RuntimeError('Authentication failed')
        # PPP control/ACK frames are small; do not coalesce them behind an
        # unacknowledged partial frame on the Windows/WSL loopback transport.
        peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        route=json.loads(subprocess.check_output(['ip','-j','route','get','223.5.5.5']))[0]
        dev=route['dev']
        if previous!='1':forward.write_text('1\n');changed=True
        tag=['-m','comment','--comment','zhaoxi-ppp-proof']
        rule('nat','POSTROUTING','-s','10.254.42.2/32','-o',dev,*tag,'-j','MASQUERADE')
        rule(None,'FORWARD','-i','ppp42','-o',dev,'-s','10.254.42.2/32',*tag,'-j','ACCEPT')
        rule(None,'FORWARD','-i',dev,'-o','ppp42','-d','10.254.42.2/32','-m','conntrack','--ctstate','ESTABLISHED,RELATED',*tag,'-j','ACCEPT')
        logfile=(root/'linux_pppd.log').open('ab')
        proc=subprocess.Popen(['pppd','notty','nodetach','noauth','local','passive','noccp','noipdefault','noipv6',
            'nodefaultroute','noproxyarp','unit','42','10.254.42.1:10.254.42.2',
            # Both UART endpoints use raw bytes without XON/XOFF. Negotiate no
            # optional control-octet escaping; PPP still escapes 0x7d and 0x7e.
            'ms-dns','223.5.5.5','mtu','512','mru','512','asyncmap','0',
            'novj','novjccomp','nopcomp','noaccomp','lcp-echo-interval','0','debug',
            'logfile',str(root/'linux_pppd.log')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=logfile,bufsize=0)
        peer.settimeout(None)
        # Finite legacy sessions allow Agent startup/negotiation and quitting.
        # Explicit persistent boot sessions have no fixed deadline; transport
        # closure, PPP exit or normal parent stop still enters the same cleanup.
        deadline=session_deadline(config)
        counts=[0,0]
        while gateway_should_run(deadline,proc):
            news_feed.tick()
            readable,_,_=select.select([peer,proc.stdout],[],[],1)
            for source in readable:
                data=peer.recv(8192) if source is peer else os.read(proc.stdout.fileno(),8192)
                if not data:raise EOFError('transport ended')
                if source is peer:
                    view=memoryview(data)
                    while view:
                        written=os.write(proc.stdin.fileno(),view)
                        if not written:raise BrokenPipeError('PPP input closed')
                        view=view[written:]
                    counts[0]+=len(data)
                else:
                    peer.sendall(data);counts[1]+=len(data)
        print(json.dumps({'transport_bytes':counts,'pppd_exit':proc.poll()}),flush=True)
    except (EOFError,ConnectionError,BrokenPipeError) as error:print(type(error).__name__,flush=True)
    finally:
        cleanup_errors=[]
        try:news_feed.close()
        except Exception as error:cleanup_errors.append('morning feed: '+str(error))
        try:
            if proc and proc.poll() is None:
                proc.terminate()
                try:proc.wait(5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
        except Exception as error:cleanup_errors.append('pppd: '+str(error))
        for command in reversed(rules):
            try:
                delete=command.copy();delete[delete.index('-I')]='-D'
                result=subprocess.run(delete,capture_output=True)
                if result.returncode:cleanup_errors.append('iptables delete exit '+str(result.returncode))
            except Exception as error:cleanup_errors.append('iptables delete: '+str(error))
        try:
            if changed and forward.read_text().strip()=='1':forward.write_text(previous+'\n')
        except Exception as error:cleanup_errors.append('ip_forward restore: '+str(error))
        if peer:peer.close()
        server.close()
        if logfile:logfile.close()
        pidfile.unlink(missing_ok=True)
        if cleanup_errors:print(json.dumps({'cleanup_errors':cleanup_errors}),flush=True)
        print('PPP_SERVER_CLEANED' if not cleanup_errors else 'PPP_SERVER_CLEANUP_FAILED',flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('--config',required=True);args=p.parse_args()
    run_gateway(args.config)


if __name__=='__main__':main()

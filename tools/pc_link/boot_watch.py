#!/usr/bin/env python3
"""Wait for the board's explicit wired selection, then attach the PC PPP link.

No Wi-Fi selection is inferred or made here. A complete selection marker or a
CRC-verified PPP LCP Configure-Request is required. The watcher releases COM
before spawning start.py, and backs off on ownership conflicts. Wireless mode
uses short read-only polling windows so normal serial tools can still operate.
This entry does not install a Windows startup task or terminate other programs.
"""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
STATE=HERE.parents[1]/'out/pc_link_boot'
MAX_EVIDENCE_AGE=15
WIRED=re.compile(rb'UPLINK_WIRED_REQUEST id=([0-9]{1,10})')


def ppp_fcs(data):
    value=0xffff
    for byte in data:
        value^=byte
        for _ in range(8):value=(value>>1)^(0x8408 if value&1 else 0)
    return value


def valid_lcp_request(raw):
    """Require a complete decoded HDLC frame, including its valid FCS."""
    if not 8<=len(raw)<=1024 or ppp_fcs(raw)!=0xf0b8:return False
    payload=raw[:-2]
    if payload.startswith(b'\xff\x03'):payload=payload[2:]
    if len(payload)<6 or payload[:3]!=b'\xc0\x21\x01':return False
    length=int.from_bytes(payload[4:6],'big')
    if length<4 or length!=len(payload)-2:return False
    options=payload[6:]
    while options:
        if len(options)<2 or options[1]<2 or options[1]>len(options):return False
        options=options[options[1]:]
    return True


class BoardSelectionParser:
    """Bounded incremental text/HDLC parser; raw board text is never logged."""
    def __init__(self):
        self.line=bytearray();self.discard_line=False
        self.frame=bytearray();self.in_frame=False;self.escaped=False;self.frame_bad=False

    def feed(self,data):
        events=[]
        for byte in data:
            if byte==10:
                if not self.discard_line:
                    line=bytes(self.line).rstrip(b'\r')
                    match=WIRED.fullmatch(line)
                    if match and 0<int(match[1])<=0xffffffff:
                        events.append({'kind':'wired-request','request_line':line.decode('ascii')})
                    elif line==b'UPLINK_WIFI_SELECTED':events.append({'kind':'wifi-selected'})
                self.line.clear();self.discard_line=False
            elif not self.discard_line:
                self.line.append(byte)
                if len(self.line)>256:self.line.clear();self.discard_line=True
            if byte==0x7e:
                if self.in_frame and not self.escaped and not self.frame_bad and valid_lcp_request(bytes(self.frame)):
                    events.append({'kind':'ppp-lcp','frame_hex':self.frame.hex()})
                self.frame.clear();self.in_frame=True;self.escaped=False;self.frame_bad=False
            elif self.in_frame and not self.frame_bad:
                if byte==0x7d:
                    if self.escaped:self.frame_bad=True
                    else:self.escaped=True
                else:
                    self.frame.append(byte^0x20 if self.escaped else byte);self.escaped=False
                    if len(self.frame)>1024:self.frame.clear();self.frame_bad=True
        return events


def validate_evidence(value,port,now=None):
    now=time.time() if now is None else now
    if not isinstance(value,dict) or type(value.get('schema')) is not int or value.get('schema')!=1 or \
       not isinstance(value.get('port'),str) or value['port'].upper()!=port.upper():
        raise RuntimeError('Boot attachment evidence does not match this serial port')
    received=value.get('received_at')
    if not isinstance(received,(int,float)) or isinstance(received,bool) or not 0<=now-received<=MAX_EVIDENCE_AGE:
        raise RuntimeError('Boot attachment evidence is missing, stale or from a future clock')
    if value.get('kind')=='wired-request':
        line=value.get('request_line')
        match=WIRED.fullmatch(line.encode('ascii')) if isinstance(line,str) and line.isascii() else None
        if match and 0<int(match[1])<=0xffffffff:return value
    elif value.get('kind')=='ppp-lcp':
        frame=value.get('frame_hex')
        if isinstance(frame,str) and len(frame)<=2048:
            try:raw=bytes.fromhex(frame)
            except ValueError:raw=b''
            if valid_lcp_request(raw):return value
    raise RuntimeError('No complete wired selection or CRC-verified board PPP request was supplied')


def read_evidence(path,port,now=None):
    if path is None:raise RuntimeError('--boot-attach requires --boot-evidence from boot_watch.py')
    try:
        if path.stat().st_size>4096:raise ValueError('size')
        value=json.loads(path.read_text(encoding='utf-8'))
    except (OSError,ValueError):raise RuntimeError('Boot attachment evidence could not be read') from None
    return validate_evidence(value,port,now)


def write_json(path,value):
    temporary=path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    for attempt in range(6):
        try:temporary.replace(path);return
        except PermissionError:
            if attempt==5:raise
            time.sleep(.02)


def listen_window(port,parser,stop_requested,seconds,clock=time.monotonic):
    """Read only. Caller owns opening/closing the port and performs handoff."""
    end=clock()+seconds
    while clock()<end and not stop_requested():
        events=parser.feed(port.read(4096))
        if events:return events[-1]
    return None


def confirm_child_cleanup(sessions,session_id,pid,port):
    """A process exit is not proof that its board, driver or gateway released."""
    from launch import read_status
    try:value=read_status(sessions/session_id/'status.json')
    except (OSError,ValueError):
        raise RuntimeError('Attached PPP session cleanup receipt is missing or unreadable') from None
    if not isinstance(value,dict) or value.get('session_id')!=session_id or \
       type(value.get('forwarder_pid')) is not int or value['forwarder_pid']!=pid or \
       value.get('port')!=port or value.get('boot_attach') is not True:
        raise RuntimeError('Attached PPP cleanup receipt does not match the exact child session')
    if value.get('phase') not in ('stopped','failed') or \
       value.get('serial_close_confirmed') is not True or \
       value.get('serial_receiver_stopped') is not True or \
       value.get('serial_cleanup_timeout') is not False or \
       value.get('gateway_cleanup_confirmed') is not True or \
       value.get('board_finished_marker_seen') is not True or \
       value.get('board_exit_line_complete') is not True or \
       value.get('board_cleanup_confirmed') is not True or \
       type(value.get('board_exit_rc')) is not int:
        raise RuntimeError('Attached PPP session cleanup or board exit was not confirmed; serial reacquisition refused')
    return value


def request_stop(state_dir,wait=250):
    from launch import process_running,read_status
    status_path=state_dir/'watch.json'
    if not status_path.exists():print('No boot network watcher is running.');return 0
    status=read_status(status_path)
    if status.get('phase')=='stopped' and status.get('cleanup_confirmed') is True:
        print('Boot network watcher is already stopped with cleanup confirmed.');return 0
    if status.get('phase')=='failed' or process_running(status.get('pid')) is False:
        raise RuntimeError('Watcher is not running but serial/session cleanup was not confirmed')
    (state_dir/'stop.request').touch()
    end=time.monotonic()+wait
    while time.monotonic()<end:
        status=read_status(status_path)
        if status.get('phase')=='stopped' and status.get('cleanup_confirmed') is True:
            print('Boot network watcher stopped with serial/session cleanup confirmed.');return 0
        if status.get('phase')=='failed':raise RuntimeError('Watcher stopped with serial/session cleanup unconfirmed')
        if process_running(status.get('pid')) is False:
            raise RuntimeError('Watcher exited without confirming serial/session cleanup')
        time.sleep(.2)
    raise RuntimeError('Watcher stop is still pending; keep its logs and check the active PPP session')


def watcher_lock(state_dir):
    """OS-owned lock: a crash releases it without deleting another PID's file."""
    handle=(state_dir/'watch.lock').open('a+b')
    try:
        handle.seek(0,os.SEEK_END)
        if not handle.tell():handle.write(b'\0');handle.flush()
        handle.seek(0)
        if os.name=='nt':
            import msvcrt
            msvcrt.locking(handle.fileno(),msvcrt.LK_NBLCK,1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
    except OSError:
        handle.close()
        raise RuntimeError('Another boot watcher owns this state directory; stop it first') from None
    return handle


def run_watch(port_name,state_dir,duration=0,serial_factory=None,popen=None,clock=time.monotonic):
    state_dir.mkdir(parents=True,exist_ok=True)
    with watcher_lock(state_dir):
        return _run_watch(port_name,state_dir,duration,serial_factory,popen,clock)


def _run_watch(port_name,state_dir,duration=0,serial_factory=None,popen=None,clock=time.monotonic):
    from launch import active_sessions,process_running
    from start import close_serial_transport
    if serial_factory is None:
        import serial
        serial_factory=serial.Serial
    popen=subprocess.Popen if popen is None else popen
    state_dir.mkdir(parents=True,exist_ok=True)
    status_path=state_dir/'watch.json';stop_path=state_dir/'stop.request';sessions=state_dir/'sessions'
    if status_path.exists():
        previous=json.loads(status_path.read_text(encoding='utf-8'))
        if previous.get('cleanup_confirmed') is not True:
            raise RuntimeError('Previous watcher cleanup is unconfirmed; inspect its exact session before restarting')
        if previous.get('phase')!='stopped' and process_running(previous.get('pid')) is not False:
            raise RuntimeError('A boot watcher is already running; use --stop before starting another')
    if active_sessions(sessions):raise RuntimeError('An attached PPP session is still active; stop it first')
    stop_path.unlink(missing_ok=True)
    parser=BoardSelectionParser();port=None;child=None;log=None;session_id=None
    release_failed=False;failed=False;child_receipt_failed=False
    status={'schema':1,'pid':os.getpid(),'port':port_name,'phase':'waiting-choice',
            'serial_open':False,'cleanup_confirmed':False,'serial_close_confirmed':True,
            'child_cleanup_confirmed':True}
    def update(**fields):status.update(fields);write_json(status_path,status)
    def stopped():return stop_path.exists()
    def release_port():
        nonlocal port,release_failed
        owned=port;port=None  # Never retry a timed-out close on the same driver.
        if owned is None:return
        result=close_serial_transport(owned,progress=lambda value:update(**value))
        update(**result)
        if result['serial_close_confirmed'] is not True or result['serial_cleanup_timeout']:
            release_failed=True
            raise RuntimeError('Watcher serial release was not confirmed; handoff/reopen refused')
        update(serial_open=False)
    def finish_child():
        nonlocal child,log,child_receipt_failed
        try:value=confirm_child_cleanup(sessions,session_id,child.pid,port_name)
        except BaseException:
            child_receipt_failed=True
            raise
        update(last_attach_rc=child.returncode,last_board_exit_rc=value['board_exit_rc'],
               child_cleanup_confirmed=True)
        child=None
        if log:log.close();log=None
    poll_mode=False
    try:
        update()
        while not stopped():
            if child:
                if child.poll() is None:
                    time.sleep(.2);continue
                finish_child()
                update(phase='waiting-choice')
                parser=BoardSelectionParser();poll_mode=False
            try:
                port=serial_factory();port.port=port_name;port.baudrate=921600
                port.timeout=.05;port.write_timeout=2;port.dtr=False;port.rts=False;port.open()
                reset=getattr(port,'reset_input_buffer',None)
                if reset:reset()  # A newly read marker must not be old driver backlog.
                update(serial_open=True,serial_close_confirmed=False,
                       phase='polling-wireless' if poll_mode else 'waiting-choice')
                event=listen_window(port,parser,stopped,1 if poll_mode else 5,clock)
            except OSError:
                update(phase='port-busy',serial_open=False)
                event=None
            finally:
                release_port()
            if event and event['kind']=='wifi-selected':
                poll_mode=True;update(phase='wireless-selected')
            elif event:
                evidence=dict(event,schema=1,port=port_name,received_at=time.time())
                validate_evidence(evidence,port_name)
                evidence_path=state_dir/'boot_evidence.json';write_json(evidence_path,evidence)
                # The serial handle is already closed. Start the downloader's
                # peer immediately; the board's PPP negotiation retries frames.
                session_id=secrets.token_hex(16)
                child_receipt_failed=False
                update(attach_session_id=session_id,child_cleanup_confirmed=False)
                command=[sys.executable,str(HERE/'start.py'),'--port',port_name,
                    '--duration',str(duration),'--state-dir',str(sessions),
                    '--boot-attach','--boot-evidence',str(evidence_path),
                    '--external-stop-request',str(stop_path),'--session-id',session_id]
                log=(state_dir/'attach.log').open('ab')
                child=popen(command,cwd=HERE.parents[1],stdout=log,stderr=subprocess.STDOUT,
                    creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                update(phase='attaching-wired',attach_pid=child.pid)
                continue
            # A released/reopened stream is not contiguous: an old partial
            # marker must never be completed using bytes from a later boot.
            parser=BoardSelectionParser()
            deadline=clock()+(3 if poll_mode or status['phase']=='port-busy' else .1)
            while clock()<deadline and not stopped():time.sleep(.1)
    except BaseException as error:
        failed=True
        update(phase='failed',cleanup_confirmed=False,error=str(error))
        raise
    finally:
        try:
            release_port()
            if child and not child_receipt_failed:
                if child.poll() is None:
                    update(phase='stopping-ppp')
                    # The external stop file exists even before child status
                    # publication; target only this child's known session.
                    stop_path.touch()
                    exact=sessions/session_id
                    if exact.is_dir():(exact/'stop.request').touch()
                    try:child.wait(timeout=250)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError('Attached PPP forwarder has not finished cleanup') from None
                finish_child()
            if release_failed:raise RuntimeError('Watcher serial release remains unconfirmed')
            if not failed and status['child_cleanup_confirmed'] is not True:
                raise RuntimeError('Attached child session cleanup remains unconfirmed')
            if not failed:
                update(phase='stopped',serial_open=False,cleanup_confirmed=True)
                stop_path.unlink(missing_ok=True)
        except BaseException as error:
            update(phase='failed',cleanup_confirmed=False,error=str(error))
            raise
        finally:
            if log:log.close()
    return 0


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port');p.add_argument('--state-dir',type=Path,default=STATE)
    p.add_argument('--duration',type=int,default=0,help='0: until stop (default), or 60..3600 seconds')
    p.add_argument('--stop',action='store_true')
    a=p.parse_args()
    if os.name!='nt':p.error('Run the watcher on the Windows computer connected to the board')
    try:
        if a.stop:return request_stop(a.state_dir.resolve())
        if not (a.duration==0 or 60<=a.duration<=3600):p.error('--duration must be 0 or 60..3600 seconds')
        from launch import preflight
        port=preflight(a.port)
        return run_watch(port,a.state_dir.resolve(),a.duration)
    except (OSError,RuntimeError,ValueError) as error:
        print('Boot network watcher: '+str(error),file=sys.stderr,flush=True);return 1


if __name__=='__main__':raise SystemExit(main())

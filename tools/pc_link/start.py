#!/usr/bin/env python3
"""Keep the board's PPP uplink running through USB2UART and this PC's WSL network.

Requires Windows Python with pyserial, Ubuntu-22.04 WSL and its ppp package.
No cloud API credentials are stored or used by this forwarder.
"""
import argparse,json,os,queue,re,secrets,socket,subprocess,threading,time
from datetime import datetime
from pathlib import Path
import serial
from control import request,wsl_path

class ControlHealth:
    """A marker starts probing; only a recent actual reply confirms control."""
    def __init__(self,probe=request,clock=time.monotonic):
        self.probe=probe;self.clock=clock;self.thread=None;self.next_check=0
        self.result=(None,False,None);self.ever_confirmed=False
        self.voice_enabled=False;self.next_voice_check=0
        self.voice_result=(None,None,None)

    def enable_voice_status(self):
        self.voice_enabled=True

    def tick(self):
        if self.thread and self.thread.is_alive():return
        ping_due=self.clock()>=self.next_check
        voice_due=self.voice_enabled and self.clock()>=self.next_voice_check
        if not ping_due and not voice_due:return
        if ping_due:self.next_check=self.clock()+30
        if voice_due:self.next_voice_check=self.clock()+4
        def run():
            if ping_due:
                try:
                    reply=self.probe({'action':'ping'},timeout=4)
                    ok=reply.get('rc')==0
                    error=None if ok else 'Board ping returned rc='+str(reply.get('rc'))
                except Exception as exc:ok=False;error=str(exc)
                if ok:self.ever_confirmed=True
                self.result=(self.clock(),ok,error)
            if voice_due:
                try:
                    reply=self.probe({'action':'auto-status'},timeout=4)
                    voice=reply.get('auto_voice')
                    if reply.get('rc')!=0 or not isinstance(voice,dict):
                        raise RuntimeError('Board auto-status failed (rc='+str(reply.get('rc'))+')')
                    self.voice_result=(self.clock(),voice,None)
                except Exception as exc:self.voice_result=(self.clock(),None,str(exc))
        self.thread=threading.Thread(target=run,daemon=True);self.thread.start()

    def snapshot(self):
        checked,ok,error=self.result
        age=self.clock()-checked if checked is not None else None
        return {'ready':bool(ok and age is not None and age<=45),
                'control_check_age_seconds':round(age,1) if age is not None else None,
                'control_check_error':error}

    def voice_snapshot(self):
        checked,voice,error=self.voice_result
        age=self.clock()-checked if checked is not None else None
        fresh=age is not None and age<=12
        if voice is not None and not fresh:error='Board automatic voice status confirmation expired'
        return {'auto_voice_board_status':voice,
                'auto_status_check_age_seconds':round(age,1) if age is not None else None,
                'auto_status_check_error':error,
                'auto_status_confirmed':bool(fresh and voice is not None and not error)}

    def wait(self):
        if self.thread:self.thread.join(25)

class AutoVoiceDisplay:
    """Only report semantic state changes, never dump speech or frame counts."""
    def __init__(self,clock=time.monotonic):
        self.clock=clock;self.previous=None;self.entered=clock();self.next_wait=15

    def message(self,snapshot):
        error=snapshot.get('auto_status_check_error')
        voice=snapshot.get('auto_voice_board_status')
        if error:
            key=('status-read-error',error)
            text='自动语音状态读取未确认：'+str(error)+'。这表示状态查询失败，不是识别结果。'
        elif not snapshot.get('auto_status_confirmed') or not isinstance(voice,dict):return None
        else:
            state=voice.get('state','未知');code=voice.get('last_error',0)
            enabled=voice.get('enabled') is True;running=voice.get('running') is True
            accepting=voice.get('accepting_speech')
            key=('board-state',enabled,running,state,code,voice.get('utterances'),accepting,
                 voice.get('wake_enabled'),voice.get('awake'))
            if not enabled and running:
                text='已请求停止收音，正在等待当前处理退出；尚未确认自动语音完全停止。'
            elif not running:
                text='板端自动语音已停止。'
                if state=='回答超时，自动监听已停止':text='本轮回答超时，自动监听已停止；请检查后主动重新启动。'
            else:
                messages={
                    '校准环境噪声':'正在校准环境噪声，请稍等。',
                    '等待新闻播放许可':'新闻已准备好，请说“播放”或“暂不播放”；60秒无回应将保留内容并返回等待唤醒。',
                    '等待唤醒词':'正在检测唤醒词，请说“你好，夕夕”。',
                    '等待提问':'可以向开发板提问。',
                    '正在播报唤醒应答':'开发板正在播报“你好”，播完后可以提问。',
                    '等待说话':'正在等待你说话，可以直接对板子提问。',
                    '正在聆听':'正在收音；说完后自然停顿，板子会自动处理。',
                    '正在识别':'正在识别本轮语音，当前暂停收音；请等回答结束后再说下一句。',
                    '等待助手回答':'本轮语音已交给助手，正在等待回答；当前暂停收音。',
                    '正在合成语音，暂停聆听':'正在生成和接收回答语音，尚未开始播放；当前暂停收音。',
                    '正在播放回答，暂停聆听':'正在播放回答，当前暂停收音；播完会继续等待说话。',
                    '播报中，暂停聆听':'正在准备或播放回答语音，当前暂停收音；播完会继续等待说话。',
                    '收音被打断，等待停顿':'本句收音被打断，正在等待停顿；听到就绪提示后请重新说完整的一句。',
                    '语句过长，等待停顿':'本句过长，已停止提交；请先停顿，听到就绪提示后分成较短的句子重新说。',
                    '语音异常，正在恢复':'本次语音处理失败，板子正在恢复。两短音表示本次失败，随后重新等待说话。'
                }
                text=messages.get(state,'板端语音状态：'+str(state))
                if state in ['等待说话','等待唤醒词','等待提问','正在聆听'] and accepting is True and voice.get('wake_enabled') is True:
                    text=('正在等待唤醒；请先说“你好，夕夕”或“Hello，xixi”，也可以接着说出问题。' if voice.get('awake') is False else
                          '朝夕已唤醒，可以直接说下一句；安静一分钟后回到等待唤醒。')
                if state in ['等待说话','等待唤醒词','等待提问','正在聆听'] and accepting is not True:
                    text=('板端正在恢复收音，尚未确认可以接收下一句话，请稍等。' if accepting is False else
                          '板端报告正在等待你说话；当前固件尚未提供实际收音确认。')
                if code:text+=' 错误码：'+str(code)+'。'
        if key!=self.previous:
            self.previous=key;self.entered=self.clock();self.next_wait=15
            return text
        if not error and enabled and running and state in ['正在识别','等待助手回答','正在合成语音，暂停聆听','正在播报唤醒应答']:
            elapsed=self.clock()-self.entered
            if elapsed>=self.next_wait:
                self.next_wait=elapsed+30
                return '当前'+{'正在识别':'语音识别','等待助手回答':'助手回答','正在合成语音，暂停聆听':'语音合成','正在播报唤醒应答':'唤醒问候'}[state]+ \
                    '阶段已等待约 '+str(int(elapsed))+' 秒，仍在处理；当前暂停收音。'
        return None


def write_status(path,value):
    """The stop launcher must never see a half-written state document."""
    temporary=path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value,indent=2),encoding='utf-8')
    for attempt in range(6):
        try:temporary.replace(path);break
        except PermissionError:
            if attempt==5:raise
            time.sleep(.01)

class NetworkClock:
    """Renew board UTC on each confirmed connection without blocking UART relay."""
    def __init__(self,send=request,clock=time.monotonic,epoch=time.time):
        self.send=send;self.clock=clock;self.epoch=epoch;self.online=False
        self.thread=None;self.retry=0;self.result={'news_clock_sync_state':'waiting-network'}

    def update(self,online):
        if not online:
            self.online=False
            return
        if self.thread and self.thread.is_alive():return
        if self.online and self.result['news_clock_sync_state']=='confirmed':return
        if self.online and self.clock()<self.retry:return
        self.online=True;self.retry=self.clock()+60
        self.result={'news_clock_sync_state':'syncing'}
        def run():
            try:
                now=int(self.epoch());reply=self.send({'action':'sync-time','epoch':now},timeout=8)
                good=reply.get('rc')==0 and isinstance(reply.get('epoch'),(int,float)) and abs(reply['epoch']-now)<=3
                self.result={'news_clock_sync_state':'confirmed' if good else 'failed'}
            except Exception:
                self.result={'news_clock_sync_state':'failed'}
        self.thread=threading.Thread(target=run,daemon=True);self.thread.start()

    def wait(self):
        if self.thread:self.thread.join(12)


class AutomaticVoice:
    """Request the board's own automatic loop once; never record on the PC."""
    def __init__(self,enabled,send=request,stop_when_disabled=False):
        self.enabled=enabled;self.stop_when_disabled=stop_when_disabled;self.send=send;self.thread=None
        self.result={'auto_voice_state':'waiting-network' if enabled or stop_when_disabled else 'disabled',
                     'auto_voice_rc':None,'auto_voice_error':None}

    def start_once(self):
        if (not self.enabled and not self.stop_when_disabled) or self.thread is not None:return
        self.result={'auto_voice_state':'starting','auto_voice_rc':None,'auto_voice_error':None}
        def run():
            try:
                reply=self.send({'action':'command','args':['auto','start' if self.enabled else 'stop']},timeout=8)
                rc=reply.get('rc')
                self.result={'auto_voice_state':('started' if self.enabled else 'stopped') if rc==0 else 'failed',
                             'auto_voice_rc':rc,
                             'auto_voice_error':None if rc==0 else 'Board auto voice policy returned rc='+str(rc)}
            except Exception as error:
                self.result={'auto_voice_state':'failed','auto_voice_rc':None,'auto_voice_error':str(error)}
        self.thread=threading.Thread(target=run,daemon=True);self.thread.start()

    def wait(self):
        if self.thread:self.thread.join(25)


class BoardOwnedVoice:
    """Boot selection firmware owns audio startup; PC only observes status."""
    def __init__(self):
        self.result={'auto_voice_state':'board-owned','auto_voice_rc':None,'auto_voice_error':None}
    def start_once(self):pass
    def wait(self):pass

def open_gateway(settings):
    link=socket.create_connection(('127.0.0.1',settings['port']),10)
    try:
        link.sendall((settings['token']+'\n').encode())
        # A UART read can contain only the first octet of a TCP ACK frame.
        # Forward the remainder immediately rather than waiting on Nagle.
        link.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        # The ten-second limit is only for connecting. An idle PPP link must
        # continue receiving the peer's next packet, including LCP keepalives.
        link.settimeout(None)
        return link
    except BaseException:
        link.close()
        raise

UART_WRITE_CHUNK = 512
# Board Agent shutdown alone has a 45-second budget. Managed PPP can still
# be in its one-second poll, then restores routes/DNS/TTY and prints the exit.
# Keep forwarding through that work; a late nonzero exit remains a failure.
BOARD_QUIT_WAIT_SECONDS = 90


def read_available(port,maximum=8192):
    """Forward small ACKs immediately instead of waiting to fill an 8-KiB read."""
    waiting=getattr(port,'in_waiting',0)
    return port.read(min(maximum,waiting) if waiting else 1)


def traffic_snapshot(metrics,now=None):
    now=time.monotonic() if now is None else now
    count=metrics['gateway_to_uart_bytes'];active=metrics.get('uart_write_seconds',0.0)
    first=metrics.get('first_uart_write_started');last=metrics.get('last_uart_write_completed')
    span=last-first if first is not None and last is not None else 0.0
    return {
        'gateway_to_uart_bytes':count,
        'uart_to_gateway_bytes':metrics['uart_to_gateway_bytes'],
        'gateway_rx_age_seconds':round(now-metrics['last_gateway_data'],1) if metrics['last_gateway_data'] is not None else None,
        'uart_rx_age_seconds':round(now-metrics['last_uart_data'],1) if metrics['last_uart_data'] is not None else None,
        'uart_write_pending':metrics['uart_write_pending'],
        'uart_write_chunk_limit':UART_WRITE_CHUNK,
        'uart_write_calls':metrics.get('uart_write_calls',0),
        'uart_write_max_bytes':metrics.get('uart_write_max_bytes',0),
        'uart_write_seconds':round(active,6),
        'uart_write_max_seconds':round(metrics.get('uart_write_max_seconds',0.0),6),
        'uart_active_bytes_per_second':round(count/active,1) if active>0 else None,
        'uart_span_bytes_per_second':round(count/span,1) if span>0 else None,
    }


def relay_inbound(link,port,stop,errors,metrics=None,expected_close=None,console_done=None):
    try:
        while not stop.is_set() and not (console_done and console_done.is_set()):
            data=link.recv(8192)
            if not data:
                if not stop.is_set() and not (expected_close and expected_close.is_set()):
                    errors.append('Gateway closed the PPP transport')
                break
            if metrics is not None:metrics['last_gateway_data']=time.monotonic()
            # Bounded writes preserve cancellation checks and let the serial
            # driver apply backpressure. Per-128-byte sleeps previously added
            # 8.192 seconds of deliberate idle time per MiB, before USB costs.
            for at in range(0,len(data),UART_WRITE_CHUNK):
                if stop.is_set() or console_done and console_done.is_set():break
                piece=data[at:at+UART_WRITE_CHUNK]
                started=time.monotonic()
                if metrics is not None:
                    metrics['uart_write_pending']=True
                    if metrics.get('first_uart_write_started') is None:metrics['first_uart_write_started']=started
                    metrics['uart_write_calls']=metrics.get('uart_write_calls',0)+1
                    metrics['uart_write_max_bytes']=max(metrics.get('uart_write_max_bytes',0),len(piece))
                try:
                    if port.write(piece)!=len(piece):raise RuntimeError('Short UART write')
                finally:
                    if metrics is not None:
                        ended=time.monotonic();elapsed=ended-started
                        metrics['uart_write_pending']=False
                        metrics['uart_write_seconds']=metrics.get('uart_write_seconds',0.0)+elapsed
                        metrics['uart_write_max_seconds']=max(metrics.get('uart_write_max_seconds',0.0),elapsed)
                        metrics['last_uart_write_completed']=ended
                if metrics is not None:metrics['gateway_to_uart_bytes']+=len(piece)
    except Exception as error:
        if not stop.is_set():errors.append(repr(error))
    finally:
        stop.set()

def close_serial_transport(port,receiver=None,timeout=3,progress=None):
    """Cancel I/O before joining/closing; a deadline never claims release.

    Windows driver calls can outlive COMMTIMEOUTS. Isolate only final cleanup
    so a stuck driver cannot hide the status or block Linux/token cleanup.
    A timed-out daemon may still hold the port: callers must not reopen it.
    """
    events=queue.Queue()
    result={'serial_close_confirmed':False,'serial_receiver_stopped':receiver is None,
            'serial_cleanup_stage':'starting','serial_cleanup_timeout':False,
            'serial_cleanup_errors':[]}
    def stage(value):
        result['serial_cleanup_stage']=value
        events.put(dict(result,serial_cleanup_errors=list(result['serial_cleanup_errors'])))
    def run():
        try:
            for name in ('cancel_write','cancel_read'):
                stage(name)
                operation=getattr(port,name,None)
                if operation:
                    try:operation()
                    except Exception as error:
                        result['serial_cleanup_errors'].append(name+': '+repr(error))
            stage('waiting-receiver')
            if receiver:
                receiver.join(timeout)
                result['serial_receiver_stopped']=not receiver.is_alive()
            if not result['serial_receiver_stopped']:
                result['serial_cleanup_timeout']=True
                result['serial_cleanup_errors'].append('UART receive thread did not stop; serial close was skipped')
                stage('receiver-still-running')
                return
            stage('closing-port')
            port.close()
            if getattr(port,'is_open',False):
                result['serial_cleanup_errors'].append('Serial close returned while the port was still open')
                stage('close-unconfirmed')
                return
            result['serial_close_confirmed']=True
            stage('closed')
        except Exception as error:
            result['serial_cleanup_errors'].append('Serial cleanup: '+repr(error))
            stage('failed')
    worker=threading.Thread(target=run,daemon=True)
    worker.start();deadline=time.monotonic()+timeout
    snapshot=dict(result,serial_cleanup_errors=[])
    while True:
        while True:
            try:snapshot=events.get_nowait()
            except queue.Empty:break
            deadline=time.monotonic()+timeout
            if progress:progress(dict(snapshot))
        if not worker.is_alive():
            if not events.empty():continue
            return snapshot
        remaining=deadline-time.monotonic()
        if remaining<=0:
            snapshot=dict(snapshot,serial_close_confirmed=False,serial_cleanup_timeout=True,
                          serial_cleanup_errors=list(snapshot['serial_cleanup_errors'])+[
                              'Serial cleanup timed out during '+snapshot['serial_cleanup_stage']+
                              '; port release was not confirmed'])
            if progress:progress(dict(snapshot))
            return snapshot
        worker.join(min(.05,remaining))


def wait_for_agent_stop(port,initial,timeout=90,clock=time.monotonic,emit=print):
    reply=bytearray(initial);started=clock();next_progress=started+10
    while b'[agent] Shutdown complete.' not in reply and clock()-started<timeout:
        reply.extend(port.read(8192))
        now=clock()
        if now>=next_progress and b'[agent] Shutdown complete.' not in reply:
            emit('仍在等待旧助手完全停止（已等待 '+str(int(now-started))+' 秒）；确认退出后才建立供网。',flush=True)
            next_progress=now+10
    return reply


def synchronize_board_clock(command,wall_clock=None):
    """Set the stopped board's UTC clock and require its explicit readback."""
    # time.time is a UTC Unix epoch; a local timezone offset must not be added.
    epoch=int((time.time if wall_clock is None else wall_clock)())
    if not 1735689600<=epoch<=4102444800:
        raise RuntimeError('电脑系统时间不在支持范围内，请先校准电脑时间。')
    reply=command('zhaoxi --set-time '+str(epoch))
    matches=re.findall(rb'(?:^|[\r\n])UTC clock set rc=(-?\d+) epoch=(\d+)(?=[\r\n]|$)',reply)
    if len(matches)!=1:
        raise RuntimeError('未收到开发板完整的授时回执，本次没有启动供网或自动语音。')
    rc,actual=map(int,matches[0])
    if rc!=0:
        raise RuntimeError('开发板授时失败，返回码 '+str(rc)+'；本次没有启动供网或自动语音。')
    if not epoch<=actual<=epoch+5:
        raise RuntimeError('开发板授时回读与电脑 UTC 时间不符，本次没有启动供网或自动语音。')
    return {'clock_sync_state':'confirmed','pc_utc_epoch':epoch,'board_utc_epoch':actual,
            'clock_sync_error':None}


def read_board_exit(port,initial,timeout=2,clock=time.monotonic):
    """Drain a split printf line without forwarding anything to the console."""
    data=bytearray(initial);before=len(data);end=clock()+timeout
    while True:
        at=data.find(b'PC_LINK finished')
        newline=data.find(b'\n',at) if at>=0 else -1
        if newline>=0:
            line=bytes(data[at:newline]).rstrip(b'\r')
            match=re.search(rb'^PC_LINK finished rc=(-?\d+)(?:;|\s|$)',line)
            cleanup=re.findall(rb'(?m)^PC_LINK_CLEANUP confirmed=([01])\r?\n',bytes(data[:at]))
            return {'seen':True,'rc':int(match.group(1)) if match else None,
                    'cleanup_confirmed':cleanup==[b'1'],
                    'line':line.decode('utf-8',errors='replace'),'extra':bytes(data[before:])}
        if clock()>=end:
            return {'seen':at>=0,'rc':None,'line':None,'cleanup_confirmed':False,'extra':bytes(data[before:])}
        data.extend(port.read(8192))

def validate_board_exit(seen,rc,control_verified):
    if not seen or rc is None:
        raise RuntimeError('未收到开发板完整的退出结果，不能确认本次会话正常结束。')
    if rc!=0:
        raise RuntimeError(('开发板联网启动失败' if not control_verified else '开发板供网会话异常结束')+
                           '，返回码 '+str(rc)+'。')
    if not control_verified:
        raise RuntimeError('开发板在控制通信验证通过前结束，本次启动失败。')

def drain_board_quit(port,link,tail,serial_log,send,capture_exit,errors,
                     metrics=None,timeout=BOARD_QUIT_WAIT_SECONDS,clock=time.monotonic):
    """Keep forwarding while the board closes capture and restores its console."""
    def request_quit():
        try:send({'action':'quit'},timeout=8)
        except Exception as error:errors.append('Quit command: '+repr(error))
    closer=threading.Thread(target=request_quit,daemon=True);closer.start()
    until=clock()+timeout
    while clock()<until:
        data=read_available(port)
        if data:
            if metrics is not None:metrics['last_uart_data']=clock()
            serial_log.write(data);serial_log.flush();tail.extend(data)
            if b'PC_LINK finished' not in tail:
                link.sendall(data)
                if metrics is not None:metrics['uart_to_gateway_bytes']+=len(data)
        if b'PC_LINK finished' in tail:
            capture_exit();break
    closer.join(.2)
    return closer


def prepare_board_transport(port,boot_attach=False):
    """Legacy launch owns NSH setup; boot attach must not write any NSH bytes."""
    if boot_attach:
        print('已确认板端有线选择；直接接入 PPP，保留固件拥有的助手和语音状态。',flush=True)
        return {'clock_sync_state':'waiting-network','clock_sync_error':None}
    port.write(b'\n');time.sleep(.2);port.read(32768)
    def command(text,timeout=6):
        port.read(8192)
        port.write(text.encode()+b'\n');end=time.monotonic()+timeout;out=bytearray()
        while time.monotonic()<end:
            out.extend(port.read(8192))
            if re.search(rb'(?:ap|nsh)>[ \t]*\x1b\[K',out):return bytes(out)
        raise TimeoutError('Board console did not respond')
    reply=command('zhaoxi status')
    if 'Agent 消息总线已启动'.encode() in reply:
        print('正在停止旧助手并等待完整退出确认；本次尚未启动新供网或自动语音。',flush=True)
        reply=wait_for_agent_stop(port,command('zhaoxi stop'))
        if b'[agent] Shutdown complete.' not in reply:
            print('未收到完整停止日志，正在核对实际任务与消息总线状态……',flush=True)
            tasks=command('ps');state=command('zhaoxi status')
            if re.search(rb'\bai_agent(?:\s|$)',tasks) or 'Agent 消息总线已启动'.encode() in state:
                raise TimeoutError('旧助手仍未完整退出，本次没有继续启动新助手或自动语音。')
            print('已核对：旧助手任务退出，消息总线停止。',flush=True)
        else:print('旧助手已确认完整停止。',flush=True)
    elif re.search(rb'\bai_agent(?:\s|$)',command('ps')):
        raise RuntimeError('旧助手总线未就绪，但主任务仍未退出；本次没有启动新会话。')
    print('旧助手已退出，正在校准开发板时间；本次尚未启动供网或自动语音。',flush=True)
    result=synchronize_board_clock(command)
    print('开发板 UTC 时间校准已确认，正在建立电脑供网，然后启动板上业务服务。',flush=True)
    port.write(b'zhaoxi pc-link serve\n')
    return result


def valid_session_duration(duration,boot_attach=False):
    return type(duration) is int and (60<=duration<=3600 or duration==0 and boot_attach is True)


def gateway_config(duration,boot_attach,token):
    if not valid_session_duration(duration,boot_attach):raise ValueError('Invalid PPP session duration')
    value={'port':19654,'token':token,'seconds':duration}
    if boot_attach:value['boot_attach']=True
    if boot_attach and duration==0:value['persistent']=True
    return value


def session_within_limit(duration,elapsed):
    # Zero reaches here only after the boot-attach duration validator succeeds.
    return duration==0 or elapsed<duration


class GatewayOutput:
    """One daemon owns pipe reads; the session thread only polls a queue."""
    def __init__(self,stream):
        self.events=queue.Queue();self.lines=[];self.eof=False;self.error=None
        def read():
            try:
                while True:
                    line=stream.readline()
                    if not line:break
                    self.events.put(('line',line))
            except Exception as error:self.events.put(('error',repr(error)))
            finally:self.events.put(('eof',None))
        self.thread=threading.Thread(target=read,daemon=True);self.thread.start()

    def poll(self):
        fresh=[]
        while True:
            try:kind,value=self.events.get_nowait()
            except queue.Empty:break
            if kind=='line':self.lines.append(value);fresh.append(value)
            elif kind=='error':self.error=value
            else:self.eof=True
        return fresh

    def wait_ready(self,worker,deadline,stop_requested,clock=time.monotonic):
        while True:
            lines=self.poll()
            if stop_requested():raise RuntimeError('Stopped during gateway startup')
            if clock()>=deadline:raise TimeoutError('Gateway readiness deadline expired')
            if worker.poll() is not None:raise RuntimeError('Gateway exited before readiness')
            if self.error:raise RuntimeError('Gateway output failed: '+self.error)
            if any(line in (b'PPP_SERVER_READY\n',b'PPP_SERVER_READY\r\n') for line in lines):return
            if self.eof:raise RuntimeError('Gateway output ended before a complete ready line')
            # Event.wait keeps stop/deadline checks responsive even if readline
            # is stuck on an empty pipe or an unterminated partial marker.
            threading.Event().wait(min(.05,max(0,deadline-clock())))

    def finish(self,timeout=3):
        self.thread.join(timeout);self.poll()
        if self.thread.is_alive() or not self.eof:
            raise RuntimeError('Gateway output EOF was not confirmed')
        if self.error:raise RuntimeError('Gateway output failed: '+self.error)
        return b''.join(self.lines).decode('utf-8',errors='replace')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',default='COM3')
    parser.add_argument('--duration',type=int,default=3600,help='Session limit, 60..3600 seconds; 0 means until stop only with --boot-attach')
    parser.add_argument('--state-dir',type=Path,default=Path(__file__).resolve().parents[2]/'out/pc_link')
    parser.add_argument('--boot-attach',action='store_true',help='Attach only after a verified board-owned wired boot request; never use NSH setup')
    parser.add_argument('--boot-evidence',type=Path,help='Fresh selection/PPP evidence created by boot_watch.py')
    parser.add_argument('--external-stop-request',type=Path,help=argparse.SUPPRESS)
    parser.add_argument('--session-id',help=argparse.SUPPRESS)
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--auto-voice',dest='auto_voice',action='store_true',help='Keep automatic wake listening enabled (default)')
    mode.add_argument('--manual-voice','--network-only',dest='auto_voice',action='store_false',help='Stop board automatic listening after control is verified')
    parser.set_defaults(auto_voice=True)
    args=parser.parse_args()
    if os.name!='nt' or not valid_session_duration(args.duration,args.boot_attach):
        parser.error('Use Windows and 60..3600 seconds; only --boot-attach supports --duration 0')
    if args.boot_attach:
        from boot_watch import read_evidence
        read_evidence(args.boot_evidence,args.port)
        if args.auto_voice is False:parser.error('--boot-attach leaves voice policy to firmware; --manual-voice is not supported')
    elif args.boot_evidence is not None:parser.error('--boot-evidence requires --boot-attach')
    if args.external_stop_request is not None and not args.boot_attach:
        parser.error('--external-stop-request is reserved for --boot-attach')
    if args.session_id is not None and (not args.boot_attach or not re.fullmatch(r'[0-9a-f]{32}',args.session_id)):
        parser.error('--session-id requires --boot-attach and a 32-character hexadecimal ID')
    if args.external_stop_request is not None and args.external_stop_request.exists():return 0
    root=args.state_dir.resolve()/(args.session_id or datetime.now().strftime('%Y%m%d_%H%M%S'))
    root.mkdir(parents=True,exist_ok=False)
    config=root/'private.json';config.write_text(json.dumps(gateway_config(args.duration,args.boot_attach,secrets.token_hex(24))))
    settings=json.loads(config.read_text())
    errors=[];stop=threading.Event();closing=threading.Event();console_done=threading.Event()
    port=None;link=None;receiver=None;ready=False;failed=False
    rpc_lock=threading.Lock()
    def serialized_request(value,timeout=90):
        with rpc_lock:
            if (closing.is_set() or stop.is_set()) and value.get('action')!='quit':
                raise RuntimeError('Session is stopping')
            return request(value,timeout=timeout)
    worker=None;gateway_output=None;stderr=None;serial_log=None;closer=None
    health=ControlHealth(probe=serialized_request)
    automatic=BoardOwnedVoice() if args.boot_attach else AutomaticVoice(args.auto_voice,send=serialized_request,stop_when_disabled=True)
    voice_display=AutoVoiceDisplay()
    if args.boot_attach:health.enable_voice_status()
    network_clock=NetworkClock(send=serialized_request)
    metrics={'gateway_to_uart_bytes':0,'uart_to_gateway_bytes':0,
             'last_gateway_data':None,'last_uart_data':None,'uart_write_pending':False}
    status={'phase':'starting','port':args.port,'ready':False,'forwarder_pid':os.getpid(),
            'session_id':args.session_id,'gateway_cleanup_confirmed':False,
            'boot_attach':args.boot_attach,'voice_owner':'board' if args.boot_attach else 'pc-launcher',
            'auto_voice_requested':args.auto_voice,'board_finished_marker_seen':False,
            'board_exit_rc':None,'board_exit_line_complete':False,'board_cleanup_confirmed':False,
            'clock_sync_state':'not-started','pc_utc_epoch':None,'board_utc_epoch':None,
            'clock_sync_error':None}
    status.update(automatic.result)
    def update():
        status['traffic']=traffic_snapshot(metrics)
        write_status(root/'status.json',status)
    update()
    started=time.monotonic();startup_deadline=started+150
    def stop_requested():
        return stop.is_set() or (root/'stop.request').exists() or \
            args.external_stop_request is not None and args.external_stop_request.exists()
    try:
        stderr=(root/'gateway_stderr.log').open('wb')
        worker=subprocess.Popen(['wsl','-d','Ubuntu-22.04','--','python3','-u',
            wsl_path(Path(__file__).with_name('gateway.py')),'--config',wsl_path(config)],
            stdout=subprocess.PIPE,stderr=stderr,creationflags=subprocess.CREATE_NO_WINDOW)
        gateway_output=GatewayOutput(worker.stdout)
        serial_log=(root/'serial_private.bin').open('wb')
        gateway_output.wait_ready(worker,startup_deadline,stop_requested)
        link=open_gateway(settings)
        if stop_requested():raise RuntimeError('Stopped before serial attachment')
        if args.boot_attach:read_evidence(args.boot_evidence,args.port)
        port=serial.Serial();port.port=args.port;port.baudrate=921600;port.timeout=.05;port.write_timeout=2
        port.dtr=False;port.rts=False;port.open()
        status.update(phase='attaching-board' if args.boot_attach else 'synchronizing-board');update()
        try:status.update(prepare_board_transport(port,args.boot_attach))
        except Exception as error:
            status.update(clock_sync_state='failed',clock_sync_error=str(error));update()
            raise
        update()
        receiver=threading.Thread(target=relay_inbound,args=(link,port,stop,errors,metrics,closing,console_done),daemon=True);receiver.start()
        print('PPP session starting; waiting for board readiness',flush=True)
        tail=bytearray();last=-10;announced=False
        last_voice_state=automatic.result['auto_voice_state'];previous_network_ready=None
        def capture_board_exit():
            closing.set();console_done.set()
            exit_result=read_board_exit(port,tail)
            if exit_result['extra']:
                serial_log.write(exit_result['extra']);serial_log.flush();tail.extend(exit_result['extra'])
            status.update(board_finished_marker_seen=exit_result['seen'],board_exit_rc=exit_result['rc'],
                          board_exit_line_complete=exit_result['line'] is not None,
                          board_cleanup_confirmed=exit_result['cleanup_confirmed'],
                          board_exit_line=exit_result['line'])
            update()
        while session_within_limit(args.duration,time.monotonic()-started) and not stop.is_set():
            if (root/'stop.request').exists() or args.external_stop_request is not None and args.external_stop_request.exists():
                print('Stop requested; returning the board to its console',flush=True)
                break
            data=read_available(port)
            if data:
                metrics['last_uart_data']=time.monotonic()
                serial_log.write(data);serial_log.flush()
                tail.extend(data)
                if len(tail)>16384:del tail[:-16384]
                if b'PC_LINK finished' not in tail:
                    link.sendall(data);metrics['uart_to_gateway_bytes']+=len(data)
            if b'PC_LINK finished' in tail:
                capture_board_exit();break
            if not ready and b'PC_LINK ready' in tail:
                ready=True;status.update(phase='checking-control',link_marker_seen=True);update()
                print('PPP link marker received; verifying the board command channel',flush=True)
            if ready:
                health.tick();status.update(health.snapshot())
                status['phase']='connected' if status['ready'] else 'control-unconfirmed'
            if status['ready'] and not announced:
                announced=True;previous_network_ready=None;update()
                print('PPP_UPLINK_READY: board command channel available',flush=True)
                print('Control with: python tools/pc_link/control.py --cmd ...',flush=True)
            network_clock.update(status['ready'])
            status.update(network_clock.result)
            if args.boot_attach and network_clock.result['news_clock_sync_state']=='confirmed':
                status['clock_sync_state']='confirmed-by-control'
            if status['ready']:automatic.start_once()
            status.update(automatic.result)
            if status['auto_voice_state'] in ['started','stopped']:health.enable_voice_status()
            status.update(health.voice_snapshot())
            voice=status.get('auto_voice_board_status') or {}
            status['usable_for_automatic_voice']=bool(status['ready'] and status['auto_status_confirmed'] and
                voice.get('enabled') is True and voice.get('running') is True and not voice.get('last_error'))
            status['accepting_speech']=bool(status['usable_for_automatic_voice'] and voice.get('accepting_speech') is True)
            if status['auto_voice_state']!=last_voice_state:
                last_voice_state=status['auto_voice_state'];update()
                if last_voice_state=='starting':
                    print('网络控制已确认，正在设置开发板自动监听状态……',flush=True)
                elif last_voice_state=='started':
                    print('板端已接受自动语音启动，正在确认实时状态。两短音表示本次失败后重新等待。',flush=True)
                elif last_voice_state=='stopped':
                    print('开发板已接受停止自动监听请求，电脑继续供网。',flush=True)
                elif last_voice_state=='failed':
                    print('自动语音未能启动：'+str(status['auto_voice_error'])+'。供网仍保留用于排查。',flush=True)
            message=voice_display.message(status) if args.auto_voice else None
            if message:
                print(message,flush=True);update()
            if previous_network_ready is True and not status['ready']:
                update()
                print('电脑到板子的控制通信未确认，当前不能视为自动语音可用；供网诊断继续运行。',flush=True)
            elif previous_network_ready is False and status['ready'] and announced:
                update()
                print('电脑到板子的控制通信已恢复。',flush=True)
            previous_network_ready=status['ready']
            elapsed=int(time.monotonic()-started)
            if not health.ever_confirmed and time.monotonic()>=startup_deadline:raise TimeoutError('Wired board control channel not verified after 150 seconds')
            if elapsed-last>=30:
                status['session_seconds']=elapsed;update()
                print('session_seconds',elapsed,'ready',status['ready'],flush=True);last=elapsed
        if stop.is_set() and not status['board_finished_marker_seen']:
            capture_board_exit()
        if status['board_finished_marker_seen']:
            validate_board_exit(True,status['board_exit_rc'],health.ever_confirmed)
            errors[:]=[e for e in errors if e!='Gateway closed the PPP transport']
        if errors:raise RuntimeError(errors[-1])
        if ((root/'stop.request').exists() or args.external_stop_request is not None and args.external_stop_request.exists()) and not ready:
            raise RuntimeError('Stopped during link setup; board console restoration has not been confirmed')
        if ready and b'PC_LINK finished' not in tail and not stop.is_set():
            closing.set()
            status.update(phase='stopping-board',ready=False,
                          usable_for_automatic_voice=False,accepting_speech=False,
                          board_quit_budget_seconds=BOARD_QUIT_WAIT_SECONDS);update()
            closer=drain_board_quit(port,link,tail,serial_log,serialized_request,
                                    capture_board_exit,errors,metrics)
        validate_board_exit(status['board_finished_marker_seen'],status['board_exit_rc'],health.ever_confirmed)
        print('开发板已正常结束供网并返回控制台。',flush=True)
    except KeyboardInterrupt:
        print('收到停止请求，正在关闭板端自动语音并等待返回控制台……',flush=True)
        closing.set()
        try:
            if ready and not status['board_finished_marker_seen'] and not stop.is_set():
                status.update(phase='stopping-board',ready=False,
                              usable_for_automatic_voice=False,accepting_speech=False,
                              board_quit_budget_seconds=BOARD_QUIT_WAIT_SECONDS);update()
                closer=drain_board_quit(port,link,tail,serial_log,serialized_request,
                                        capture_board_exit,errors,metrics)
            validate_board_exit(status['board_finished_marker_seen'],status['board_exit_rc'],health.ever_confirmed)
            print('开发板已确认结束供网并返回控制台。',flush=True)
        except (Exception,KeyboardInterrupt) as error:
            failed=True;errors.append('Interrupted before board shutdown confirmation: '+repr(error))
            print('尚未确认板端停止，请检查开发板和本机状态记录。',flush=True)
    except Exception as error:
        failed=True
        if not errors or errors[-1]!=str(error):errors.append(str(error))
        print('供网未能正常完成：'+str(error),flush=True)
        raise
    finally:
        stop.set();closing.set()
        def cleanup(label,operation):
            try:operation()
            except Exception as error:errors.append(label+': '+str(error))
        status.update(phase='closing-serial',ready=False,usable_for_automatic_voice=False,
                      accepting_speech=False,serial_close_confirmed=port is None,
                      serial_receiver_stopped=receiver is None,
                      serial_cleanup_stage='pending' if port else 'not-opened',
                      serial_cleanup_timeout=False)
        cleanup('Record serial cleanup state',update)
        if link:
            try:link.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            cleanup('Close gateway socket',link.close)
        if port:
            def serial_progress(value):
                status.update(value)
                cleanup('Record serial cleanup progress',update)
            serial_result=close_serial_transport(port,receiver,progress=serial_progress)
            status.update(serial_result)
            errors.extend(serial_result['serial_cleanup_errors'])
        elif receiver:cleanup('Stop receive thread',lambda:receiver.join(3))
        if serial_log:cleanup('Close serial log',serial_log.close)
        cleanup('Wait for control probe',health.wait)
        cleanup('Wait for automatic voice start request',automatic.wait)
        cleanup('Wait for network clock request',network_clock.wait)
        if closer:cleanup('Wait for quit request',lambda:closer.join(25))
        if worker:
            def stop_gateway():
                try:worker.wait(10)
                except subprocess.TimeoutExpired:
                    linux_pid=wsl_path(root/'pc_ppp_server.pid')
                    script="from pathlib import Path; import os,signal; p=Path("+repr(linux_pid)+"); pid=int(p.read_text()) if p.exists() else 0; cmd=Path('/proc/'+str(pid)+'/cmdline').read_bytes() if pid else b''; os.kill(pid,signal.SIGTERM) if pid and b'gateway.py' in cmd else None"
                    subprocess.run(['wsl','-d','Ubuntu-22.04','--','python3','-c',script],capture_output=True,timeout=15,creationflags=subprocess.CREATE_NO_WINDOW,check=True)
                    worker.wait(10)
                if gateway_output is None:raise RuntimeError('Gateway output reader was not started')
                summary=gateway_output.finish()
                if worker.stdout:worker.stdout.close()
                (root/'gateway_summary.log').write_text(summary,encoding='utf-8')
                status['gateway_cleanup_confirmed']='PPP_SERVER_CLEANED' in summary.splitlines() and 'CLEANUP_FAILED' not in summary
                if not status['gateway_cleanup_confirmed']:
                    raise RuntimeError('Gateway cleanup was not confirmed; inspect gateway logs')
            cleanup('Stop Linux gateway and restore forwarding rules',stop_gateway)
        if stderr:cleanup('Close gateway log',stderr.close)
        cleanup('Remove private channel token',lambda:config.unlink(missing_ok=True))
        status.update(phase='failed' if failed or errors else 'stopped',ready=False,
                      usable_for_automatic_voice=False,accepting_speech=False,errors=errors);update()
        print('PPP session stopped; logs:',root,flush=True)
    return 1 if failed or errors else 0

if __name__=='__main__':raise SystemExit(main())

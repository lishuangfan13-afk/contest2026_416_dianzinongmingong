#!/usr/bin/env python3
"""Mirror BES activity as fixed emotions on one CH343 EasyMatrix display.

No microphone commands, firmware updates, or access to the BES FTDI serial
port. Text, recordings, credentials, and raw log lines never go to the display.
"""
import argparse
import json
import sys
import threading
import time
from pathlib import Path
import re

STATES = ('idle', 'listening', 'thinking', 'speaking', 'success', 'error', 'sleep', 'clock')
MAX_LINE = 192
ACK_MAX_LINE = 512
TTL_MS = 10000
RENEW_SECONDS = 5
SESSION_ROOT = Path(__file__).resolve().parents[1] / 'out/pc_link'


class ProtocolError(RuntimeError):
    pass


class NotReadyError(ProtocolError):
    """No matching ACK yet; only this condition permits a boot retry."""


class LegacyProtocolError(ProtocolError):
    pass


def choose_port(ports, requested=None):
    ports = list(ports)
    if requested:
        selected = next((p for p in ports if p.device.upper() == requested.upper()), None)
        if not selected:
            raise RuntimeError('没有找到指定的颜文字屏串口。')
        if (selected.vid, selected.pid) != (0x1A86, 0x55D3):
            raise RuntimeError('指定串口不是 CH343 颜文字屏；没有打开该端口。')
        return selected.device
    candidates = [p.device for p in ports if (p.vid, p.pid) == (0x1A86, 0x55D3)]
    if len(candidates) != 1:
        raise RuntimeError('需要唯一的 CH343 颜文字屏；有多块时请用 --port 指定。')
    return candidates[0]


def _integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


class DisplayClient:
    def __init__(self, port, clock=time.monotonic, ack_timeout=1.5):
        self.port = port
        self.clock = clock
        self.ack_timeout = ack_timeout
        self.next_id = 1

    def request(self, cmd, state=None, ttl_ms=TTL_MS, timeout=None):
        if timeout is not None and not 0 < timeout <= 90:
            raise ValueError('Invalid display response budget')
        original = {name: getattr(self.port, name) for name in ('timeout', 'write_timeout')
                    if hasattr(self.port, name)}
        deadline = self.clock() + timeout if timeout is not None else None
        try:
            if timeout is not None and 'write_timeout' in original:
                limit = original['write_timeout']
                if limit is None or limit > timeout:
                    self.port.write_timeout = timeout
            return self._request(cmd, state, ttl_ms, deadline)
        finally:
            for name, value in original.items():
                if getattr(self.port, name) != value:
                    setattr(self.port, name, value)

    def _request(self, cmd, state, ttl_ms, deadline):
        if cmd not in ('ping', 'status', 'emotion'):
            raise ValueError('Unsupported display command')
        if cmd == 'emotion' and (state not in STATES or not _integer(ttl_ms) or ttl_ms != TTL_MS):
            raise ValueError('Unsupported display emotion or lease')
        if cmd != 'emotion' and state is not None:
            raise ValueError('State is only valid for emotion')
        ident = self.next_id
        self.next_id = ident + 1 if ident < 2147483647 else 1
        value = {'id': ident, 'cmd': cmd}
        if cmd == 'emotion':
            value.update(state=state, ttl_ms=ttl_ms)
        wire = json.dumps(value, ensure_ascii=True, separators=(',', ':')).encode('ascii') + b'\n'
        if len(wire) > MAX_LINE:
            raise ValueError('Display request exceeds the line limit')
        if deadline is not None and self.clock() >= deadline:
            raise NotReadyError('颜文字屏启动等待时间已到，尚未收到完整确认。')
        if self.port.write(wire) != len(wire):
            raise ProtocolError('颜文字屏串口写入不完整；没有重发。')
        if deadline is None:
            deadline = self.clock() + self.ack_timeout
        while self.clock() < deadline:
            left = deadline - self.clock()
            if left <= 0:
                break
            current = getattr(self.port, 'timeout', None)
            if hasattr(self.port, 'timeout') and (current is None or current > left):
                self.port.timeout = left
            line = self.port.readline(ACK_MAX_LINE + 1)
            if not line:
                continue
            if len(line) > ACK_MAX_LINE or not line.endswith(b'\n'):
                # Do not interpret old CLI output or an oversized partial line.
                continue
            if (line.strip().startswith('可用命令：'.encode('utf-8')) and
                b'config' in line and b'icons' in line and b'JSON ping/status/emotion' not in line):
                raise LegacyProtocolError('颜文字屏仍运行旧 CLI 菜单程序，需要匹配的 JSON 固件。没有刷写固件。')
            try:
                reply = json.loads(line.decode('ascii'))
            except (ValueError, UnicodeError):
                continue
            if (isinstance(reply, dict) and reply.get('type') == 'zhaoxi.display' and
                (reply.get('v') != 1 or reply.get('ok') is False and reply.get('id') is None)):
                raise ProtocolError('颜文字屏返回了明确的协议版本或请求错误。')
            if not isinstance(reply, dict) or reply.get('id') != ident:
                continue
            if (not _integer(reply.get('id')) or reply.get('type') != 'zhaoxi.display' or
                not _integer(reply.get('v')) or reply.get('v') != 1):
                raise ProtocolError('颜文字屏协议版本或确认标识不匹配。')
            if reply.get('ok') is not True or reply.get('cmd') != cmd:
                raise ProtocolError('颜文字屏没有接受这条显示请求。')
            if (reply.get('device') != 'EasyMatrix' or
                not _integer(reply.get('width')) or reply.get('width') != 32 or
                not _integer(reply.get('height')) or reply.get('height') != 8 or
                reply.get('state') not in STATES or not isinstance(reply.get('active'), bool) or
                not _integer(reply.get('frame')) or reply['frame'] < 0 or
                not _integer(reply.get('uptime_ms')) or reply['uptime_ms'] < 0):
                raise ProtocolError('颜文字屏确认缺少有效的设备与显示状态。')
            if cmd == 'emotion' and (reply['state'] != state or reply['active'] is not (state != 'clock')):
                raise ProtocolError('颜文字屏确认的画面与请求不一致。')
            return reply
        raise NotReadyError('颜文字屏尚未返回匹配的 JSON 确认。没有刷写固件。')

    def handshake(self, deadline=None):
        reply = None
        for cmd in ('ping', 'status'):
            if deadline is None:
                reply = self.request(cmd)
            else:
                remaining = deadline - self.clock()
                if remaining <= 0:
                    raise NotReadyError('颜文字屏启动等待时间已到，尚未收到完整确认。')
                reply = self.request(cmd, timeout=min(self.ack_timeout, remaining))
        return reply


def wait_for_display_ready(client, boot_wait_seconds=70, clock=time.monotonic,
                           sleep=time.sleep, emit=print):
    """Wait for cold boot with bounded, sparse probes, without resetting ESP."""
    if not 0 <= boot_wait_seconds <= 90:
        raise ValueError('Display boot wait must be 0..90 seconds')
    deadline = clock() + boot_wait_seconds
    announced = False
    while True:
        try:
            return client.handshake(deadline=deadline if boot_wait_seconds else None)
        except NotReadyError:
            remaining = deadline - clock()
            if not boot_wait_seconds or remaining <= 0:
                raise NotReadyError('颜文字屏启动等待已结束，仍未收到完整 ping/status 确认。') from None
            if not announced:
                emit('颜文字屏可能正在冷启动和联网对时，正在自动等待（最多 '+
                     str(boot_wait_seconds)+' 秒），无需按 RESET。')
                announced = True
            sleep(min(5, remaining))


_PREFIX = rb'(?:^|~)(?:\[[^\]\r\n]{1,64}\]\s*){0,4}'
_TRACE = re.compile(_PREFIX + rb'\[trace:([0-9a-fA-F]{8,32})\]\s+(BEGIN|END)\b([^\r\n]*)')
_RESULT = re.compile(_PREFIX + rb'(?:TODO_RESULT|TO_DO_RESULT|NOTE_RESULT|DRINK_RESULT|REMINDER_DELIVERY)\b([^\r\n]*)')
_REMINDER = re.compile(_PREFIX + rb'\[Reminder\]:')


def parse_trace(data):
    """Return metadata tuples only; never retain or return any message body."""
    events = []
    for line in data.split(b'\n'):
        trace = _TRACE.search(line)
        if trace:
            ident = trace[1].decode('ascii')
            if trace[2] == b'BEGIN':
                events.append(('begin', ident))
            else:
                status = re.search(rb'\bstatus=(ok|fail|failed|error|timeout)\b', trace[3])
                if status:
                    events.append(('end', (ident, status[1].decode('ascii'))))
            continue
        result = _RESULT.search(line)
        if result:
            code = re.search(rb'\brc=(-?\d+)\b', result[1])
            if code:
                events.append(('result', 'success' if int(code[1]) == 0 else 'error'))
        elif _REMINDER.search(line):
            events.append(('reminder', 'success'))
    return events


class SerialEvents:
    def __init__(self, path):
        self.path = Path(path)
        self.offset = self.path.stat().st_size if self.path.exists() else 0
        self.tail = b''

    def read_new(self):
        try:
            with self.path.open('rb') as stream:
                stream.seek(0, 2)
                if stream.tell() < self.offset:
                    self.offset = 0
                    self.tail = b''
                stream.seek(self.offset)
                data = stream.read(65536)
                self.offset = stream.tell()
        except OSError:
            return []
        combined = self.tail + data
        end = combined.rfind(b'\n')
        if end < 0:
            self.tail = combined[-4096:]
            return []
        self.tail = combined[end + 1:][-4096:]
        return parse_trace(combined[:end + 1])


class StateMapper:
    def __init__(self):
        self.traces = {}
        self.overlay = None
        self.overlay_until = 0
        self.rpc_failed_since = None

    def consume(self, events, now):
        for kind, value in events:
            if kind == 'begin':
                if len(self.traces) >= 64:
                    self.traces.clear()
                self.traces[value] = now
                self.overlay = None
            elif kind == 'end':
                ident, status = value
                self.traces.pop(ident, None)
                self.overlay = 'success' if status == 'ok' else 'error'
                self.overlay_until = now + 3
            elif kind in ('result', 'reminder'):
                self.overlay = value
                self.overlay_until = now + 3

    def desired(self, metadata, now, rpc_failed=False, session_ready=True):
        if not session_ready:
            self.rpc_failed_since = None
            return 'clock'
        if rpc_failed:
            if self.rpc_failed_since is None:
                self.rpc_failed_since = now
            return 'error' if now - self.rpc_failed_since < 2 else 'clock'
        self.rpc_failed_since = None
        state = metadata if isinstance(metadata, dict) else {}
        label = str(state.get('state', ''))
        if state.get('output_playing') is True or '正在播放' in label:
            return 'speaking'
        if state.get('output_active') is True:
            return 'thinking'
        if self.overlay and now < self.overlay_until:
            return self.overlay
        self.traces = {k: v for k, v in self.traces.items() if now - v < 120}
        if self.traces:
            return 'thinking'
        if state.get('wake_enabled') is True and state.get('awake') is not True:
            return 'sleep'
        if state.get('enabled') is True and state.get('running') is True:
            if any(word in label for word in ('识别', '等待助手', '合成')):
                return 'thinking'
            if any(word in label for word in ('异常', '失败', '超时')) or state.get('last_error'):
                return 'error'
            if state.get('accepting_speech') is True or any(word in label for word in ('聆听', '等待说话', '校准')):
                return 'listening'
        return 'idle'


def readonly_request(value, timeout=2):
    from pc_link.control import request
    return request(value, timeout=timeout)


class ReadOnlyPoller:
    """One read-only RPC in flight; slow WSL startup never stalls TTL renewal."""
    def __init__(self, request=readonly_request, clock=time.monotonic):
        self.request = request
        self.clock = clock
        self.thread = None
        self.generation = 0
        self.next_poll = 0
        self.started = None
        self.result = None

    def reset(self):
        self.generation += 1
        self.result = None
        self.next_poll = 0
        self.started = None

    def tick(self, ready=True):
        if not ready or (self.thread and self.thread.is_alive()) or self.clock() < self.next_poll:
            return
        generation = self.generation
        self.started = self.clock()
        self.next_poll = self.started + 2
        def run():
            try:
                reply = self.request({'action': 'auto-status'}, timeout=2)
                metadata = reply.get('auto_voice')
                if reply.get('rc') != 0 or not isinstance(metadata, dict):
                    raise ValueError('Invalid read-only status')
                result = (metadata, False, self.clock())
            except Exception:
                result = (None, True, self.clock())
            if generation == self.generation:
                self.result = (generation, *result)
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def snapshot(self):
        if self.started is not None and self.thread and self.thread.is_alive() and self.clock() - self.started > 4:
            return None, True
        if self.result is None:
            return None, False
        generation, metadata, failed, when = self.result
        if generation != self.generation:
            return None, False
        return (None, True) if self.clock() - when > 6 else (metadata, failed)


def session_snapshot(root, explicit=None, alive=None):
    from pc_link.launch import process_running, read_status
    alive = alive or process_running
    candidates = [Path(explicit) / 'status.json'] if explicit else sorted(Path(root).glob('*/status.json'), reverse=True)
    for path in candidates:
        try:
            value = read_status(path)
        except (OSError, ValueError):
            continue
        if not isinstance(value, dict) or value.get('phase') in ('stopped', 'failed'):
            continue
        if alive(value.get('forwarder_pid')) is False:
            continue
        return path.parent, value
    return None, None


class DisplayBridge:
    def __init__(self, client, session_root=SESSION_ROOT, watch_session=None,
                 request=readonly_request, clock=time.monotonic, loader=session_snapshot):
        self.client = client
        self.root = session_root
        self.explicit = watch_session
        self.clock = clock
        self.loader = loader
        self.poller = ReadOnlyPoller(request, clock)
        self.mapper = StateMapper()
        self.session = None
        self.reader = None
        self.last_state = None
        self.last_sent = float('-inf')

    def step(self):
        now = self.clock()
        directory, status = self.loader(self.root, self.explicit)
        if directory != self.session:
            self.session = directory
            self.reader = SerialEvents(directory / 'serial_private.bin') if directory else None
            self.mapper = StateMapper()
            self.poller.reset()
        ready = bool(status and status.get('ready'))
        if self.reader:
            self.mapper.consume(self.reader.read_new(), now)
        self.poller.tick(ready)
        metadata, failed = self.poller.snapshot()
        if metadata is None and status and status.get('auto_status_confirmed'):
            metadata = status.get('auto_voice_board_status')
        desired = self.mapper.desired(metadata, now, failed, ready)
        if desired != self.last_state or now - self.last_sent >= RENEW_SECONDS:
            self.client.request('emotion', desired)
            self.last_state = desired
            self.last_sent = now
        return desired

    def close(self):
        self.poller.reset()
        self.client.request('emotion', 'clock')


def open_display_port(name):
    import serial
    port = serial.Serial()
    port.port = name
    port.baudrate = 115200
    port.bytesize = 8
    port.parity = 'N'
    port.stopbits = 1
    port.timeout = .1
    port.write_timeout = 1
    port.xonxoff = port.rtscts = port.dsrdtr = False
    port.dtr = False
    port.rts = False
    port.open()
    return port


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='Select one CH343 display; FTDI/BES ports are refused')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--show', choices=STATES)
    mode.add_argument('--watch-session', type=Path)
    parser.add_argument('--session-root', type=Path, default=SESSION_ROOT)
    parser.add_argument('--hold-seconds', type=float, default=5, help='Single-state test duration, 0..30 seconds')
    parser.add_argument('--boot-wait-seconds', type=float, default=70,
                        help='Wait for display cold boot, 0..90 seconds; 0 disables retries')
    args = parser.parse_args(argv)
    if not 0 <= args.hold_seconds <= 30:
        parser.error('--hold-seconds must be 0..30')
    if not 0 <= args.boot_wait_seconds <= 90:
        parser.error('--boot-wait-seconds must be 0..90')
    port = None
    client = None
    verified = False
    try:
        from serial.tools import list_ports
        selected = choose_port(list_ports.comports(), args.port)
        port = open_display_port(selected)
        client = DisplayClient(port)
        wait_for_display_ready(client, args.boot_wait_seconds, clock=client.clock, sleep=time.sleep)
        verified = True
        print('颜文字屏已确认：EasyMatrix 32×8，串口 '+selected+'。', flush=True)
        if args.show:
            client.request('emotion', args.show)
            until = time.monotonic() + args.hold_seconds
            renew_at = time.monotonic() + RENEW_SECONDS
            while time.monotonic() < until:
                if time.monotonic() >= renew_at:
                    client.request('emotion', args.show)
                    renew_at = time.monotonic() + RENEW_SECONDS
                time.sleep(min(.1, max(0, until - time.monotonic())))
        else:
            bridge = DisplayBridge(client, args.session_root, args.watch_session)
            print('正在跟随朝夕状态；这里只读状态，不会开启麦克风。Ctrl+C 结束并恢复时钟。', flush=True)
            while True:
                bridge.step()
                time.sleep(.2)
        return 0
    except KeyboardInterrupt:
        return 0
    except (ImportError, OSError, RuntimeError, ValueError) as error:
        # Error messages contain controlled protocol diagnostics, never raw lines.
        print('颜文字屏未能继续：'+str(error), file=sys.stderr, flush=True)
        return 1
    finally:
        if verified and client:
            try:
                client.request('emotion', 'clock')
            except (OSError, RuntimeError, ValueError):
                print('未收到恢复时钟确认；显示租约将在10秒内到期。', file=sys.stderr, flush=True)
        if port:
            port.close()


if __name__ == '__main__':
    raise SystemExit(main())

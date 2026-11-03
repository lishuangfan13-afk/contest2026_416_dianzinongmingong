"""Focused host regression tests for the persistent PPP byte relay."""
import socket
import threading
import time
import unittest
import tempfile
import json
import os
import io
import contextlib
import ast
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace

from start import ControlHealth, UART_WRITE_CHUNK, drain_board_quit, main as start_main, open_gateway, read_available, relay_inbound, read_board_exit, synchronize_board_clock, traffic_snapshot, validate_board_exit, write_status
from control import main as control_main, process_error, request
from launch import read_status


class SerialSink:
    def __init__(self, short=False):
        self.received = bytearray()
        self.arrived = threading.Event()
        self.short = short

    def write(self, data):
        self.received.extend(data)
        self.arrived.set()
        return len(data) - 1 if self.short else len(data)


class RelayTests(unittest.TestCase):
    def test_tcp_nodelay_is_enabled_on_both_host_and_wsl_sockets(self):
        calls=[]
        class Link:
            def sendall(self,data):calls.append(('auth',data))
            def setsockopt(self,*args):calls.append(('option',args))
            def settimeout(self,value):calls.append(('timeout',value))
        link=Link()
        with patch('start.socket.create_connection',return_value=link):
            self.assertIs(open_gateway({'port':123,'token':'test-token'}),link)
        option=(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        self.assertEqual(calls,[('auth',b'test-token\n'),('option',option),('timeout',None)])
        # Evaluate only the accepted-peer socket option, not gateway setup.
        module=ast.parse(Path(__file__).with_name('gateway.py').read_text(encoding='utf-8'))
        accepted=[node for node in ast.walk(module) if isinstance(node,ast.Call) and
                  isinstance(node.func,ast.Attribute) and node.func.attr=='setsockopt' and
                  isinstance(node.func.value,ast.Name) and node.func.value.id=='peer']
        self.assertEqual(len(accepted),1)
        calls.clear()
        eval(compile(ast.Expression(accepted[0]),'gateway-nodelay','eval'),
             {'peer':link,'socket':socket})
        self.assertEqual(calls,[('option',option)])

    def test_tcp_nodelay_failure_closes_transport(self):
        closed=[]
        class Link:
            def sendall(self,data):pass
            def setsockopt(self,*args):raise OSError('socket option failed')
            def close(self):closed.append(True)
        with patch('start.socket.create_connection',return_value=Link()):
            with self.assertRaisesRegex(OSError,'socket option failed'):
                open_gateway({'port':123,'token':'test-token'})
        self.assertEqual(closed,[True])

    def test_gateway_negotiates_raw_uart_without_optional_control_escaping(self):
        # Inspect only the subprocess argument expression; importing this script
        # would start a real gateway and mutate network rules.
        module=ast.parse(Path(__file__).with_name('gateway.py').read_text(encoding='utf-8'))
        calls=[node for node in ast.walk(module) if isinstance(node,ast.Call) and
               isinstance(node.func,ast.Attribute) and node.func.attr=='Popen']
        self.assertEqual(len(calls),1)
        command=eval(compile(ast.Expression(calls[0].args[0]),'gateway-command','eval'),
                     {'root':Path('test-session')})
        self.assertEqual(command[0],'pppd')
        self.assertEqual(command[command.index('asyncmap')+1],'0')
        self.assertNotIn('escape',command)
        self.assertEqual(command[command.index('mtu')+1],'512')
        self.assertEqual(command[command.index('mru')+1],'512')
        self.assertIn('noccp',command);self.assertIn('novj',command)

    def test_large_transfer_is_byte_exact_bounded_and_has_no_fixed_sleep(self):
        payload=bytes(range(256))*1025
        chunks=iter([payload,b''])
        class Link:
            def recv(self,_):return next(chunks)
        class Sink(SerialSink):
            def __init__(self):super().__init__();self.sizes=[]
            def write(self,data):self.sizes.append(len(data));return super().write(data)
        sink=Sink();errors=[];stop=threading.Event();closing=threading.Event();closing.set()
        metrics={'gateway_to_uart_bytes':0,'uart_to_gateway_bytes':0,
                 'last_gateway_data':None,'last_uart_data':None,'uart_write_pending':False}
        ticks=iter(index*.001 for index in range(2*len(payload)+10))
        with patch('start.time.sleep',side_effect=AssertionError('relay must not insert fixed delays')), \
             patch('start.time.monotonic',side_effect=lambda:next(ticks)):
            relay_inbound(Link(),sink,stop,errors,metrics,expected_close=closing)
        self.assertEqual(sink.received,payload)
        self.assertEqual(errors,[]);self.assertTrue(stop.is_set())
        self.assertEqual(max(sink.sizes),512)
        self.assertEqual(len(sink.sizes),(len(payload)+511)//512)
        self.assertEqual(metrics['gateway_to_uart_bytes'],len(payload))
        self.assertEqual(metrics['uart_write_calls'],len(sink.sizes))
        self.assertFalse(metrics['uart_write_pending'])
        snapshot=traffic_snapshot(metrics)
        self.assertEqual(snapshot['uart_write_chunk_limit'],512)
        self.assertEqual(snapshot['uart_write_max_bytes'],512)
        self.assertGreater(snapshot['uart_write_seconds'],0)
        self.assertGreater(snapshot['uart_active_bytes_per_second'],0)

    def test_serial_backpressure_does_not_prefetch_more_network_data(self):
        entered=threading.Event();release=threading.Event();reads=[];errors=[];stop=threading.Event()
        class Link:
            def recv(self,_):
                reads.append(1)
                return b'x'*1024 if len(reads)==1 else b''
        class Sink(SerialSink):
            def write(self,data):
                entered.set();release.wait(2);return super().write(data)
        sink=Sink();closing=threading.Event();closing.set()
        worker=threading.Thread(target=relay_inbound,args=(Link(),sink,stop,errors),kwargs={'expected_close':closing})
        try:
            worker.start();self.assertTrue(entered.wait(1))
            self.assertEqual(len(reads),1)
            self.assertEqual(sink.received,b'')
        finally:
            release.set();worker.join(2)
        self.assertFalse(worker.is_alive());self.assertEqual(errors,[])
        self.assertEqual(sink.received,b'x'*1024)

    def test_cancellation_and_console_restore_stop_at_a_bounded_write(self):
        for restoring_console in (False,True):
            with self.subTest(restoring_console=restoring_console):
                stop=threading.Event();done=threading.Event();errors=[];reads=[]
                class Link:
                    def recv(self,_):reads.append(1);return b'x'*8192
                class Sink(SerialSink):
                    def write(self,data):
                        (done if restoring_console else stop).set()
                        return super().write(data)
                sink=Sink();relay_inbound(Link(),sink,stop,errors,console_done=done)
                self.assertEqual(sink.received,b'x'*UART_WRITE_CHUNK)
                self.assertEqual(reads,[1]);self.assertEqual(errors,[])

    def test_uart_exception_clears_pending_and_stops_without_retry(self):
        class Link:
            def recv(self,_):return b'frame'
        class Sink:
            def write(self,_):raise OSError('write failed')
        metrics={'gateway_to_uart_bytes':0,'uart_to_gateway_bytes':0,
                 'last_gateway_data':None,'last_uart_data':None,'uart_write_pending':False}
        errors=[];stop=threading.Event();relay_inbound(Link(),Sink(),stop,errors,metrics)
        self.assertTrue(stop.is_set());self.assertFalse(metrics['uart_write_pending'])
        self.assertEqual(metrics['gateway_to_uart_bytes'],0)
        self.assertEqual(metrics['uart_write_calls'],1)
        self.assertIn('write failed',errors[0])

    def test_read_available_waits_only_for_first_ack_byte(self):
        sizes=[]
        class Port:
            in_waiting=0
            def read(self,count):sizes.append(count);return b'A'
        port=Port();self.assertEqual(read_available(port),b'A')
        port.in_waiting=3;read_available(port)
        port.in_waiting=20000;read_available(port)
        self.assertEqual(sizes,[1,3,8192])

    def test_read_available_legacy_test_port_also_waits_for_one_byte(self):
        class Port:
            def read(self,count):self.count=count;return b'ack'
        port=Port();self.assertEqual(read_available(port),b'ack');self.assertEqual(port.count,1)

    def test_traffic_snapshot_supports_zero_clock_origin_and_idle_session(self):
        metrics={'gateway_to_uart_bytes':0,'uart_to_gateway_bytes':0,
                 'last_gateway_data':None,'last_uart_data':None,'uart_write_pending':False}
        value=traffic_snapshot(metrics,now=0)
        self.assertIsNone(value['uart_active_bytes_per_second']);self.assertIsNone(value['uart_span_bytes_per_second'])
        metrics.update(gateway_to_uart_bytes=100,uart_write_seconds=.5,
                       first_uart_write_started=0,last_uart_write_completed=2,last_gateway_data=0)
        value=traffic_snapshot(metrics,now=3)
        self.assertEqual(value['uart_active_bytes_per_second'],200)
        self.assertEqual(value['uart_span_bytes_per_second'],50)
        self.assertEqual(value['gateway_rx_age_seconds'],3)

    def test_idle_connection_still_receives_later_ppp_frame(self):
        # Reproduce the failure without a ten-second test: the connection
        # deadline is short, but the gateway's next packet arrives later.
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        original_connect = socket.create_connection
        peers = []
        expected = b'\x7e\xff\x03\xc0\x21later PPP frame\x7e'

        def gateway():
            peer, _ = listener.accept()
            peers.append(peer)
            auth = bytearray()
            while not auth.endswith(b'\n'):
                auth.extend(peer.recv(1))
            self.assertEqual(bytes(auth), b'local-test-token\n')
            time.sleep(.15)
            peer.sendall(expected)

        server = threading.Thread(target=gateway)
        server.start()
        stop = threading.Event()
        errors = []
        link = None
        receiver = None
        try:
            def short_connect(address, timeout):
                return original_connect(address, .03)
            with patch('start.socket.create_connection', side_effect=short_connect):
                link = open_gateway({'port': listener.getsockname()[1], 'token': 'local-test-token'})
            sink = SerialSink()
            receiver = threading.Thread(target=relay_inbound, args=(link, sink, stop, errors))
            receiver.start()
            self.assertTrue(sink.arrived.wait(2), errors)
            self.assertEqual(sink.received, expected)
            self.assertFalse(stop.is_set())
            self.assertEqual(errors, [])
            server.join(2)
            peers[0].shutdown(socket.SHUT_RDWR)
            peers[0].close()
            receiver.join(2)
            self.assertTrue(stop.is_set())
            self.assertIn('Gateway closed', errors[-1])
        finally:
            stop.set()
            if link:
                link.shutdown(socket.SHUT_RDWR)
                link.close()
            for peer in peers:
                peer.close()
            listener.close()
            server.join(2)
            if receiver:
                receiver.join(2)

    def test_short_uart_write_stops_session(self):
        link, peer = socket.socketpair()
        stop = threading.Event()
        errors = []
        sink = SerialSink(short=True)
        receiver = threading.Thread(target=relay_inbound, args=(link, sink, stop, errors))
        try:
            receiver.start()
            peer.sendall(b'frame')
            receiver.join(2)
            self.assertFalse(receiver.is_alive())
            self.assertTrue(stop.is_set())
            self.assertIn('Short UART write', errors[-1])
        finally:
            stop.set()
            peer.close()
            link.close()

    def test_intentional_shutdown_is_not_reported_as_failure(self):
        link, peer = socket.socketpair()
        stop = threading.Event()
        errors = []
        stop.set()
        try:
            relay_inbound(link, SerialSink(), stop, errors)
            self.assertEqual(errors, [])
        finally:
            peer.close()
            link.close()


class ControlTests(unittest.TestCase):
    def test_auto_status_reads_state_without_a_microphone_command(self):
        state={'rc':0,'auto_voice':{'enabled':False,'running':False,'wake_enabled':True,
                                  'awake':False,'wake_phrase':'你好，openvela'}}
        output=io.StringIO()
        with patch('sys.argv',['control.py','--auto-status']), \
             patch('control.request',return_value=state) as send,contextlib.redirect_stdout(output):
            self.assertEqual(control_main(),0)
        send.assert_called_once_with({'action':'auto-status'},90)
        self.assertEqual(json.loads(output.getvalue()),state)

    def test_auto_status_is_exclusive_with_other_actions(self):
        combinations=[['--auto-status','--'+name] for name in
                      ['status','quit','notes','tasks','reminders','time']]
        combinations.extend([['--auto-status','--cmd','auto','start'],
                             ['--auto-status','--encoded','eyJhY3Rpb24iOiJwaW5nIn0='],
                             ['--status','--auto-status'],[]])
        for options in combinations:
            with self.subTest(options=options),patch('sys.argv',['control.py']+options), \
                 patch('control.request') as send,contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:control_main()
                self.assertEqual(stopped.exception.code,2)
                send.assert_not_called()

    def test_auto_status_preserves_board_failure(self):
        state={'rc':-5}
        output=io.StringIO()
        with patch('sys.argv',['control.py','--auto-status']), \
             patch('control.request',return_value=state) as send,contextlib.redirect_stdout(output):
            self.assertEqual(control_main(),1)
        send.assert_called_once_with({'action':'auto-status'},90)
        self.assertEqual(json.loads(output.getvalue()),state)

    def test_auto_status_transport_failure_returns_without_retrying(self):
        error=io.StringIO()
        with patch('sys.argv',['control.py','--auto-status']), \
             patch('control.request',side_effect=TimeoutError('status timed out')) as send, \
             contextlib.redirect_stderr(error):
            self.assertEqual(control_main(),2)
        send.assert_called_once_with({'action':'auto-status'},90)
        self.assertIn('status timed out',error.getvalue())

    def test_nonzero_business_result_is_preserved(self):
        child=SimpleNamespace(returncode=1,stdout=b'{"rc":-2,"tool_output":"File not found"}',
                              stderr='wsl: localhost proxy warning\r\n'.encode('utf-16-le'))
        with patch('control.os.name','nt'),patch('control.wsl_path',return_value='/test/control.py'), \
             patch('control.subprocess.run',return_value=child):
            self.assertEqual(request({'action':'notes'}),{'rc':-2,'tool_output':'File not found'})

    def test_mixed_wsl_warning_and_traceback_gives_concise_error(self):
        warning='wsl: localhost proxy warning\r\n'.encode('utf-16-le')
        error=b'Traceback (most recent call last):\n  test\nTimeoutError: timed out\n'
        self.assertEqual(process_error(warning+error,1),'TimeoutError: timed out')

    def test_non_json_child_failure_is_transport_error(self):
        child=SimpleNamespace(returncode=1,stdout=b'',stderr=b'ConnectionRefusedError: connection refused\n')
        with patch('control.os.name','nt'),patch('control.wsl_path',return_value='/test/control.py'), \
             patch('control.subprocess.run',return_value=child):
            with self.assertRaisesRegex(RuntimeError,'ConnectionRefusedError: connection refused'):
                request({'action':'ping'})


class ClockSyncTests(unittest.TestCase):
    EPOCH=1793538000

    def test_utc_epoch_is_sent_without_local_timezone_offset(self):
        calls=[]
        def command(text):
            calls.append(text)
            return ('UTC clock set rc=0 epoch='+str(self.EPOCH)+'\r\nap> \x1b[K').encode()
        with patch.dict(os.environ,{'TZ':'Pacific/Honolulu'}),patch('start.time.time',return_value=self.EPOCH+.9):
            result=synchronize_board_clock(command)
        self.assertEqual(calls,['zhaoxi --set-time '+str(self.EPOCH)])
        self.assertEqual(result['pc_utc_epoch'],self.EPOCH)
        self.assertEqual(result['board_utc_epoch'],self.EPOCH)
        self.assertEqual(result['clock_sync_state'],'confirmed')

    def test_short_readback_delay_is_accepted(self):
        result=synchronize_board_clock(lambda _:('UTC clock set rc=0 epoch='+str(self.EPOCH+2)+'\n').encode(),lambda:self.EPOCH)
        self.assertEqual(result['board_utc_epoch'],self.EPOCH+2)

    def test_echo_or_old_firmware_prompt_is_not_success(self):
        for reply in [b'zhaoxi --set-time 1793538000\r\nap> \x1b[K',
                      b'nsh: zhaoxi: command not found\n',
                      b'echo UTC clock set rc=0 epoch=1793538000\n']:
            with self.subTest(reply=reply),self.assertRaisesRegex(RuntimeError,'授时回执'):
                synchronize_board_clock(lambda _:reply,lambda:self.EPOCH)

    def test_failure_or_inconsistent_clock_is_rejected(self):
        for rc,actual in [(-5,self.EPOCH),(0,self.EPOCH-1),(0,self.EPOCH+20)]:
            with self.subTest(rc=rc,actual=actual),self.assertRaises(RuntimeError):
                synchronize_board_clock(lambda _:f'UTC clock set rc={rc} epoch={actual}\n'.encode(),lambda:self.EPOCH)

    def test_invalid_computer_clock_sends_no_board_command(self):
        calls=[]
        for epoch in [0,1735689599,4102444801]:
            with self.subTest(epoch=epoch),self.assertRaisesRegex(RuntimeError,'电脑系统时间'):
                synchronize_board_clock(lambda s:calls.append(s),lambda:epoch)
        self.assertEqual(calls,[])

    def test_timeout_is_not_treated_as_clock_confirmation(self):
        def command(_):raise TimeoutError('NSH timeout')
        with self.assertRaises(TimeoutError):synchronize_board_clock(command,lambda:self.EPOCH)


class BoardExitTests(unittest.TestCase):
    def test_quit_keeps_relay_alive_until_board_exit_and_does_not_forward_to_console(self):
        requested=threading.Event();forwarded=threading.Event();calls=[];captured=[];errors=[]
        tail=bytearray();serial_log=io.BytesIO()
        class Port:
            def __init__(self):self.step=0
            def read(self,_):
                requested.wait(1);self.step+=1
                return b'PPP reply' if self.step==1 else b'PC_LINK finished rc=0; console restored.\n'
        class Link:
            def sendall(self,data):
                calls.append(data);forwarded.set()
        def send(value,**options):
            self.assertEqual(value,{'action':'quit'});requested.set()
            self.assertTrue(forwarded.wait(1));return {'rc':0}
        closer=drain_board_quit(Port(),Link(),tail,serial_log,send,
                                lambda:captured.append(True),errors,timeout=2)
        closer.join(1)
        self.assertFalse(closer.is_alive())
        self.assertEqual(calls,[b'PPP reply'])
        self.assertEqual(captured,[True]);self.assertEqual(errors,[])
        self.assertIn(b'PC_LINK finished rc=0',serial_log.getvalue())

    def test_quit_request_without_exit_marker_does_not_invent_confirmation(self):
        now=[0];captured=[]
        class Port:
            def read(self,_):now[0]+=.5;return b''
        closer=drain_board_quit(Port(),None,bytearray(),io.BytesIO(),lambda *a,**k:{'rc':0},
                               lambda:captured.append(True),[],timeout=2,clock=lambda:now[0])
        closer.join(1)
        self.assertEqual(captured,[])

    def test_status_replace_does_not_expose_half_json_to_readers(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'status.json';write_status(path,{'phase':'starting'})
            stop=threading.Event();errors=[];seen=[]
            def read():
                while not stop.is_set():
                    try:seen.append(read_status(path)['phase'])
                    except Exception as error:errors.append(error)
                    time.sleep(.001)  # Far faster than the real stop window's 200ms poll.
            reader=threading.Thread(target=read);reader.start()
            try:
                for index in range(100):write_status(path,{'phase':'connected','sample':index})
            finally:stop.set();reader.join(2)
            self.assertEqual(errors,[]);self.assertTrue(seen)
            self.assertEqual(json.loads(path.read_text())['sample'],99)
            self.assertFalse(path.with_suffix('.tmp').exists())

    def test_split_negative_exit_line_is_read_completely(self):
        chunks=iter([b' rc=',b'-113; result /tmp/pc_link_result.json;',b' console restored.\n'])
        class Port:
            def read(self,_):return next(chunks,b'')
        result=read_board_exit(Port(),b'\nPC_LINK finished')
        self.assertEqual(result['rc'],-113)
        self.assertTrue(result['line'].endswith('console restored.'))
        with self.assertRaisesRegex(RuntimeError,'-113'):
            validate_board_exit(result['seen'],result['rc'],False)

    def test_zero_exit_before_control_verification_is_failure(self):
        with self.assertRaisesRegex(RuntimeError,'启动失败'):
            validate_board_exit(True,0,False)

    def test_complete_verified_normal_exit_is_accepted(self):
        validate_board_exit(True,0,True)

    def test_incomplete_exit_line_is_not_assumed_successful(self):
        now=[0.0]
        class Port:
            def read(self,_):now[0]+=.5;return b''
        result=read_board_exit(Port(),b'PC_LINK finished rc=0',clock=lambda:now[0])
        self.assertTrue(result['seen'])
        self.assertIsNone(result['rc'])
        with self.assertRaises(RuntimeError):
            validate_board_exit(result['seen'],result['rc'],True)

    def test_ppp_bytes_are_not_written_after_console_restore(self):
        class Forbidden:
            def recv(self,_):raise AssertionError('must not read more network frames')
            def write(self,_):raise AssertionError('must not write PPP to the console')
        done=threading.Event();done.set();stop=threading.Event();errors=[]
        relay_inbound(Forbidden(),Forbidden(),stop,errors,console_done=done)
        self.assertEqual(errors,[])


class HealthTests(unittest.TestCase):
    def test_ready_requires_reply_and_expires(self):
        now=[0.0]
        health=ControlHealth(probe=lambda *a,**k:{'rc':0},clock=lambda:now[0])
        self.assertFalse(health.snapshot()['ready'])
        health.tick();health.thread.join(1)
        self.assertTrue(health.snapshot()['ready'])
        now[0]=46
        self.assertFalse(health.snapshot()['ready'])

    def test_failed_check_replaces_previous_success(self):
        now=[0.0]
        responses=iter([{'rc':0},{'rc':-1}])
        health=ControlHealth(probe=lambda *a,**k:next(responses),clock=lambda:now[0])
        health.tick();health.thread.join(1)
        self.assertTrue(health.snapshot()['ready'])
        now[0]=30
        health.tick();health.thread.join(1)
        self.assertFalse(health.snapshot()['ready'])
        self.assertIn('rc=-1',health.snapshot()['control_check_error'])

    def test_probe_does_not_overlap_or_block_relay_loop(self):
        release=threading.Event();calls=[]
        def probe(*a,**k):
            calls.append(1);release.wait(1);return {'rc':0}
        health=ControlHealth(probe=probe)
        try:
            health.tick();health.tick();health.tick()
            self.assertFalse(health.snapshot()['ready'])
            self.assertEqual(len(calls),1)
        finally:
            release.set();health.thread.join(2)


@unittest.skipUnless(os.name=='nt','Windows launcher cleanup')
class StartupCleanupTests(unittest.TestCase):
    def test_keyboard_interrupt_requests_board_quit_before_transport_cleanup(self):
        requested=threading.Event();closed=threading.Event();operations=[]
        class Health(ControlHealth):
            def tick(self):self.result=(self.clock(),True,None);self.ever_confirmed=True
        class Port:
            def __init__(self):self.command=None;self.ppp=False;self.reads=0
            def open(self):pass
            def write(self,data):
                if data==b'zhaoxi pc-link serve\n':self.ppp=True;operations.append('serve')
                elif data.strip():self.command=data.strip()
                return len(data)
            def read(self,_):
                if self.command:
                    if self.command.startswith(b'zhaoxi --set-time '):
                        epoch=self.command.split()[-1];self.command=None;operations.append('clock-confirmed')
                        return b'UTC clock set rc=0 epoch='+epoch+b'\nap> \x1b[K'
                    self.command=None;return b'not started\nap> \x1b[K'
                if not self.ppp:return b''
                self.reads+=1
                if self.reads==1:return b'PC_LINK ready\n'
                if self.reads==2:raise KeyboardInterrupt()
                self.assert_quit()
                return b'PC_LINK finished rc=0; console restored.\n'
            def assert_quit(self):
                if not requested.wait(1):raise AssertionError('quit not requested')
            def close(self):operations.append('serial-close')
        class Link:
            def recv(self,_):closed.wait(2);return b''
            def sendall(self,data):operations.append('relay')
            def shutdown(self,_):operations.append('transport-close');closed.set()
            def close(self):closed.set()
        class Gateway:
            def __init__(self):self.stdout=io.BytesIO(b'PPP_SERVER_READY\nPPP_SERVER_CLEANED\n')
            def poll(self):return None
            def wait(self,*a):return 0
        def request(value,**options):
            self.assertEqual(value,{'action':'quit'})
            operations.append('board-quit');requested.set();return {'rc':0}
        with tempfile.TemporaryDirectory() as temp:
            with patch('sys.argv',['start.py','--duration','60','--state-dir',temp]), \
                 patch('start.ControlHealth',Health),patch('start.serial.Serial',Port), \
                 patch('start.open_gateway',return_value=Link()), \
                 patch('start.request',side_effect=request),patch('start.subprocess.Popen',return_value=Gateway()), \
                 patch('start.time.sleep'),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(start_main(),0)
            status=json.loads(next(Path(temp).glob('*/status.json')).read_text())
            self.assertEqual(status['phase'],'stopped')
            self.assertEqual(status['board_exit_rc'],0)
            self.assertTrue(status['gateway_cleanup_confirmed'])
            self.assertLess(operations.index('board-quit'),operations.index('transport-close'))
            self.assertLess(operations.index('clock-confirmed'),operations.index('serve'))
            self.assertEqual(status['clock_sync_state'],'confirmed')
            self.assertFalse(status['accepting_speech'])

    def test_clock_failure_closes_transport_and_never_starts_ppp_or_voice(self):
        for mode in ['failed','echo','mismatch','timeout']:
            with self.subTest(mode=mode):
                operations=[]
                class Port:
                    def __init__(self):self.command=None
                    def open(self):pass
                    def write(self,data):
                        operations.append(data)
                        if data.strip():self.command=data.strip()
                        return len(data)
                    def read(self,_):
                        if not self.command:return b''
                        command=self.command;self.command=None
                        if command.startswith(b'zhaoxi --set-time '):
                            epoch=int(command.split()[-1])
                            if mode=='timeout':raise TimeoutError('NSH timeout')
                            if mode=='echo':return command+b'\nap> \x1b[K'
                            if mode=='failed':return f'UTC clock set rc=-5 epoch={epoch}\nap> \x1b[K'.encode()
                            return f'UTC clock set rc=0 epoch={epoch+30}\nap> \x1b[K'.encode()
                        return b'not started\nap> \x1b[K'
                    def close(self):operations.append('serial-close')
                class Link:
                    def shutdown(self,_):operations.append('transport-close')
                    def close(self):operations.append('link-close')
                class Gateway:
                    def __init__(self):self.stdout=io.BytesIO(b'PPP_SERVER_READY\nPPP_SERVER_CLEANED\n')
                    def poll(self):return None
                    def wait(self,*args):return 0
                with tempfile.TemporaryDirectory() as temp:
                    with patch('sys.argv',['start.py','--auto-voice','--duration','60','--state-dir',temp]), \
                         patch('start.serial.Serial',Port),patch('start.open_gateway',return_value=Link()), \
                         patch('start.request') as request_mock,patch('start.subprocess.Popen',return_value=Gateway()), \
                         patch('start.time.sleep'),contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaises((RuntimeError,TimeoutError)):start_main()
                    request_mock.assert_not_called()
                    status=json.loads(next(Path(temp).glob('*/status.json')).read_text())
                    self.assertEqual(status['phase'],'failed');self.assertFalse(status['ready'])
                    self.assertEqual(status['clock_sync_state'],'failed')
                    self.assertNotEqual(status['auto_voice_state'],'started')
                    self.assertTrue(status['gateway_cleanup_confirmed'])
                    self.assertFalse(next(Path(temp).glob('*')).joinpath('private.json').exists())
                self.assertNotIn(b'zhaoxi pc-link serve\n',operations)
                self.assertIn('serial-close',operations);self.assertIn('transport-close',operations)

    def test_clock_sync_follows_confirmed_agent_shutdown(self):
        operations=[]
        class Port:
            def __init__(self):self.command=None;self.stopped=False
            def open(self):pass
            def write(self,data):
                if data.strip():self.command=data.strip()
                return len(data)
            def read(self,_):
                if not self.command:return b''
                command=self.command;self.command=None
                if command==b'zhaoxi status':return 'Agent 消息总线已启动\nap> \x1b[K'.encode()
                if command==b'zhaoxi stop':operations.append('stop-request');return b'Stopping\nap> \x1b[K'
                if command.startswith(b'zhaoxi --set-time '):
                    self.assert_stopped();operations.append('clock-request')
                    return b'UTC clock set rc=-5 epoch='+command.split()[-1]+b'\nap> \x1b[K'
                raise AssertionError('unexpected command')
            def assert_stopped(self):
                if not self.stopped:raise AssertionError('clock set before Agent exit')
            def close(self):pass
        port=Port()
        def completed_shutdown(actual,reply):
            self.assertIs(actual,port);port.stopped=True;operations.append('stop-confirmed')
            return reply+b'\n[agent] Shutdown complete.\n'
        class Link:
            def shutdown(self,_):pass
            def close(self):pass
        class Gateway:
            def __init__(self):self.stdout=io.BytesIO(b'PPP_SERVER_READY\nPPP_SERVER_CLEANED\n')
            def poll(self):return None
            def wait(self,*args):return 0
        with tempfile.TemporaryDirectory() as temp:
            with patch('sys.argv',['start.py','--duration','60','--state-dir',temp]), \
                 patch('start.serial.Serial',return_value=port),patch('start.wait_for_agent_stop',side_effect=completed_shutdown), \
                 patch('start.open_gateway',return_value=Link()),patch('start.subprocess.Popen',return_value=Gateway()), \
                 patch('start.time.sleep'),contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError,'授时失败'):start_main()
        self.assertEqual(operations,['stop-request','stop-confirmed','clock-request'])

    def test_wsl_launch_failure_removes_private_token(self):
        with tempfile.TemporaryDirectory() as temp:
            with patch('sys.argv',['start.py','--state-dir',temp]), \
                 patch('start.subprocess.Popen',side_effect=OSError('launcher failure')):
                with self.assertRaisesRegex(OSError,'launcher failure'):
                    start_main()
            sessions=list(Path(temp).iterdir())
            self.assertEqual(len(sessions),1)
            self.assertFalse((sessions[0]/'private.json').exists())
            status=json.loads((sessions[0]/'status.json').read_text())
            self.assertEqual(status['phase'],'failed')
            self.assertFalse(status['ready'])


if __name__ == '__main__':
    unittest.main()

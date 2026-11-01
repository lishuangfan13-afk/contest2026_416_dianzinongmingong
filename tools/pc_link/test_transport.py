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
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace

from start import ControlHealth, drain_board_quit, main as start_main, open_gateway, relay_inbound, read_board_exit, validate_board_exit, write_status
from control import process_error, request
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
                if data==b'zhaoxi pc-link serve\n':self.ppp=True
                elif data.strip():self.command=data.strip()
                return len(data)
            def read(self,_):
                if self.command:
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
            self.assertFalse(status['accepting_speech'])

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

"""Synthetic lifecycle failures only: no real COM, WSL, network or subprocess."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import boot_watch
import start


def receipt(session_id,pid=7654321):
    return dict(session_id=session_id,forwarder_pid=pid,port='COM20',boot_attach=True,
        phase='stopped',serial_close_confirmed=True,serial_receiver_stopped=True,
        serial_cleanup_timeout=False,gateway_cleanup_confirmed=True,
        board_finished_marker_seen=True,board_exit_line_complete=True,
        board_cleanup_confirmed=True,board_exit_rc=0)


def write_receipt(root,session_id,value):
    path=root/'sessions'/session_id/'status.json';path.parent.mkdir(parents=True)
    path.write_text(json.dumps(value),encoding='utf-8')


class GatewayStartupTests(unittest.TestCase):
    def test_complete_ready_line_and_summary_use_one_reader(self):
        output=start.GatewayOutput(io.BytesIO(b'noise\nPPP_SERVER_READY\r\nPPP_SERVER_CLEANED\n'))
        output.wait_ready(SimpleNamespace(poll=lambda:None),time.monotonic()+1,lambda:False)
        self.assertIn('PPP_SERVER_CLEANED',output.finish())

    def test_partial_ready_at_eof_is_rejected(self):
        output=start.GatewayOutput(io.BytesIO(b'PPP_SERVER_READY'))
        output.thread.join(1)
        with self.assertRaisesRegex(RuntimeError,'complete ready line'):
            output.wait_ready(SimpleNamespace(poll=lambda:None),time.monotonic()+1,lambda:False)

    def test_empty_or_partial_blocked_pipe_can_timeout_stop_or_notice_worker_exit(self):
        for cause in ('deadline','external-stop','session-stop','worker-exit'):
            with self.subTest(cause=cause),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);release=threading.Event();entered=threading.Event()
                class Pipe:
                    def readline(self):entered.set();release.wait(2);return b''
                output=start.GatewayOutput(Pipe());self.assertTrue(entered.wait(1))
                if cause.endswith('stop'):(root/(cause+'.request')).touch()
                def stopped():return any(root.glob('*.request'))
                worker=SimpleNamespace(poll=lambda:3 if cause=='worker-exit' else None)
                try:
                    began=time.monotonic()
                    with self.assertRaises((RuntimeError,TimeoutError)):
                        output.wait_ready(worker,time.monotonic()+.04,stopped)
                    self.assertLess(time.monotonic()-began,.5)
                finally:release.set();output.thread.join(1)

    def test_gateway_exit_after_ready_is_rejected_and_blocked_eof_is_bounded(self):
        output=start.GatewayOutput(io.BytesIO(b'PPP_SERVER_READY\n'))
        output.thread.join(1)
        with self.assertRaisesRegex(RuntimeError,'exited before readiness'):
            output.wait_ready(SimpleNamespace(poll=lambda:0),time.monotonic()+1,lambda:False)
        release=threading.Event()
        class Pipe:
            def readline(self):release.wait(2);return b''
        output=start.GatewayOutput(Pipe())
        try:
            began=time.monotonic()
            with self.assertRaisesRegex(RuntimeError,'EOF was not confirmed'):output.finish(timeout=.03)
            self.assertLess(time.monotonic()-began,.5)
        finally:release.set();output.thread.join(1)

    def test_read_error_is_reported(self):
        class Pipe:
            def readline(self):raise OSError('synthetic pipe failure')
        output=start.GatewayOutput(Pipe());output.thread.join(1)
        with self.assertRaisesRegex(RuntimeError,'synthetic pipe failure'):
            output.wait_ready(SimpleNamespace(poll=lambda:None),time.monotonic()+1,lambda:False)

    @unittest.skipUnless(os.name=='nt','Windows main argument guard')
    def test_startup_timeout_and_stop_run_real_main_finally(self):
        for reason in ('timeout','external-stop','session-stop','worker-exit','partial-eof'):
            with self.subTest(reason=reason),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);external=root/'external.request';release=threading.Event()
                operations=[];session_id='a'*32
                class Pipe:
                    def readline(self):
                        if reason=='partial-eof':
                            if not hasattr(self,'sent'):self.sent=True;return b'PPP_SERVER_READY'
                            return b''
                        release.wait(2);return b''
                    def close(self):operations.append('pipe-close')
                class Worker:
                    stdout=Pipe()
                    def poll(self):return 7 if reason=='worker-exit' else None
                    def wait(self,*args):release.set();return 0
                def spawn(*args,**kwargs):
                    if reason=='external-stop':external.touch()
                    if reason=='session-stop':(root/session_id/'stop.request').touch()
                    return Worker()
                original=start.GatewayOutput.wait_ready
                def short_wait(output,worker,deadline,stopped):
                    return original(output,worker,time.monotonic()+.04,stopped)
                args=['start.py','--port','COM20','--boot-attach','--boot-evidence',str(root/'evidence'),
                      '--state-dir',str(root),'--session-id',session_id,'--external-stop-request',str(external)]
                try:
                    with patch.object(sys,'argv',args),patch('boot_watch.read_evidence'), \
                         patch.object(start.subprocess,'Popen',side_effect=spawn), \
                         patch.object(start.GatewayOutput,'wait_ready',short_wait), \
                         patch.object(start.serial,'Serial',side_effect=AssertionError('COM forbidden')), \
                         patch.object(start,'open_gateway',side_effect=AssertionError('network forbidden')), \
                         patch.object(start.subprocess,'run',side_effect=AssertionError('WSL forbidden')), \
                         contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaises((RuntimeError,TimeoutError)):start.main()
                finally:release.set()
                state=json.loads((root/session_id/'status.json').read_text())
                self.assertEqual(state['phase'],'failed');self.assertFalse(state['gateway_cleanup_confirmed'])
                self.assertTrue(state['serial_close_confirmed']);self.assertIn('pipe-close',operations)
                self.assertFalse((root/session_id/'private.json').exists())


class BoardCleanupProtocolTests(unittest.TestCase):
    def test_split_cleanup_and_nonzero_finished_are_captured_exactly(self):
        wire=b'\nPC_LINK_CLEANUP confirmed=1\r\nPC_LINK finished rc=-101; console restored.\n'
        for split in range(len(wire)):
            chunks=iter([wire[split:]])
            port=SimpleNamespace(read=lambda size:next(chunks,b''))
            value=start.read_board_exit(port,wire[:split])
            self.assertEqual(value['rc'],-101);self.assertTrue(value['cleanup_confirmed'])

    def test_missing_quoted_partial_conflicting_or_late_cleanup_is_rejected(self):
        end=b'PC_LINK finished rc=-101; console restored.\n'
        for prefix,suffix in ((b'',b''),(b'quoted PC_LINK_CLEANUP confirmed=1\n',b''),
            (b'PC_LINK_CLEANUP confirmed=0\n',b''),(b'PC_LINK_CLEANUP confirmed=1',b''),
            (b'PC_LINK_CLEANUP confirmed=1 trailing\n',b''),
            (b'PC_LINK_CLEANUP confirmed=0\nPC_LINK_CLEANUP confirmed=1\n',b''),
            (b'PC_LINK_CLEANUP confirmed=1\nPC_LINK_CLEANUP confirmed=0\n',b''),
            (b'PC_LINK_CLEANUP confirmed=1\nPC_LINK_CLEANUP confirmed=1\n',b''),
            (b'',b'PC_LINK_CLEANUP confirmed=1\n')):
            value=start.read_board_exit(None,prefix+end+suffix)
            self.assertFalse(value['cleanup_confirmed'])


class WatcherFailureTests(unittest.TestCase):
    def test_nonzero_network_exit_with_cleanup_reenters_waiting(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);opens=[]
            class Port:
                is_open=False
                def open(self):self.is_open=True;opens.append(1)
                def read(self,size):
                    if len(opens)==1:return b'UPLINK_WIRED_REQUEST id=1\n'
                    (root/'stop.request').touch();return b'UPLINK_WIFI_SELECTED\n'
                def close(self):self.is_open=False
            def spawn(command,**kwargs):
                sid=command[command.index('--session-id')+1]
                write_receipt(root,sid,dict(receipt(sid),phase='failed',board_exit_rc=-101))
                return SimpleNamespace(pid=7654321,returncode=1,poll=lambda:1)
            boot_watch.run_watch('COM20',root,serial_factory=Port,popen=spawn)
            state=json.loads((root/'watch.json').read_text())
            self.assertEqual(opens,[1,1]);self.assertEqual(state['last_board_exit_rc'],-101)
            self.assertEqual(state['last_attach_rc'],1);self.assertTrue(state['cleanup_confirmed'])

    def test_bad_child_receipt_is_checked_once_without_exception_chain(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            class Port:
                is_open=False
                def open(self):self.is_open=True
                def read(self,size):return b'UPLINK_WIRED_REQUEST id=1\n'
                def close(self):self.is_open=False
            child=SimpleNamespace(pid=7654321,returncode=1,poll=lambda:1)
            with patch.object(boot_watch,'confirm_child_cleanup',side_effect=RuntimeError('bad receipt')) as check:
                try:boot_watch.run_watch('COM20',root,serial_factory=Port,popen=lambda *a,**k:child)
                except RuntimeError as error:
                    self.assertEqual(str(error),'bad receipt');self.assertIsNone(error.__context__)
                else:self.fail('bad receipt accepted')
            check.assert_called_once()

    def test_owned_sources_keep_lf(self):
        base=Path(__file__).resolve().parent
        for path in (base/'start.py',base/'boot_watch.py',base/'test_transport.py',
                     Path(__file__),base.parents[1]/'tests/headless/test_boot_pc_attach.py'):
            with self.subTest(path=path):self.assertNotIn(b'\r',path.read_bytes())

    def test_child_timeout_and_spawn_exception_keep_failed_receipt(self):
        for mode in ('timeout','spawn-error'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);opens=[]
                class Port:
                    is_open=False
                    def open(self):self.is_open=True;opens.append(1)
                    def read(self,size):return b'UPLINK_WIRED_REQUEST id=1\n'
                    def close(self):self.is_open=False
                class Child:
                    pid=7654321;returncode=None
                    def poll(self):return None
                    def wait(self,timeout):raise subprocess.TimeoutExpired('synthetic child',timeout)
                def spawn(*args,**kwargs):
                    if mode=='spawn-error':raise OSError('synthetic spawn failure')
                    (root/'stop.request').touch();return Child()
                with self.assertRaises((RuntimeError,OSError)):
                    boot_watch.run_watch('COM20',root,serial_factory=Port,popen=spawn)
                state=json.loads((root/'watch.json').read_text())
                self.assertEqual(opens,[1]);self.assertEqual(state['phase'],'failed')
                self.assertFalse(state['cleanup_confirmed'])

    def test_missing_wrong_or_incomplete_exact_receipt_is_rejected(self):
        session_id='b'*32
        variants=[None,dict(receipt(session_id),session_id='c'*32),dict(receipt(session_id),forwarder_pid=99)]
        for field in ('serial_close_confirmed','serial_receiver_stopped','gateway_cleanup_confirmed',
                      'board_finished_marker_seen','board_exit_line_complete','board_cleanup_confirmed'):
            variants.append(dict(receipt(session_id),**{field:False}))
        old=receipt(session_id);old.pop('board_cleanup_confirmed')
        variants.extend([old,dict(receipt(session_id),serial_cleanup_timeout=True),
                         dict(receipt(session_id),board_cleanup_confirmed=1),
                         dict(receipt(session_id),board_exit_rc=None),dict(receipt(session_id),board_exit_rc=False)])
        for value in variants:
            with self.subTest(value=value),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                if value is not None:write_receipt(root,session_id,value)
                with self.assertRaises(RuntimeError):
                    boot_watch.confirm_child_cleanup(root/'sessions',session_id,7654321,'COM20')

    def test_child_failure_or_missing_receipt_never_reopens_and_stop_reports_failure(self):
        for mode in ('missing','gateway','board','serial','wrong-pid'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);opens=[]
                class Port:
                    is_open=False
                    def open(self):self.is_open=True;opens.append(1)
                    def read(self,size):return b'UPLINK_WIRED_REQUEST id=1\n'
                    def close(self):self.is_open=False
                def spawn(command,**kwargs):
                    sid=command[command.index('--session-id')+1];value=receipt(sid)
                    if mode=='gateway':value['gateway_cleanup_confirmed']=False
                    if mode=='board':value['board_exit_line_complete']=False
                    if mode=='serial':value['serial_close_confirmed']=False
                    if mode=='wrong-pid':value['forwarder_pid']=99
                    if mode!='missing':write_receipt(root,sid,value)
                    return SimpleNamespace(pid=7654321,returncode=1,poll=lambda:1)
                with self.assertRaises(RuntimeError):
                    boot_watch.run_watch('COM20',root,serial_factory=Port,popen=spawn)
                self.assertEqual(opens,[1])
                state=json.loads((root/'watch.json').read_text())
                self.assertEqual(state['phase'],'failed');self.assertFalse(state['cleanup_confirmed'])
                with self.assertRaises(RuntimeError):boot_watch.request_stop(root,wait=.01)
                with self.assertRaisesRegex(RuntimeError,'Previous watcher cleanup'):
                    boot_watch.run_watch('COM20',root,serial_factory=Port,popen=spawn)

    def test_stop_waits_only_exact_child_and_requires_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);waited=[]
            class Port:
                is_open=False
                def open(self):self.is_open=True
                def read(self,size):return b'UPLINK_WIRED_REQUEST id=1\n'
                def close(self):self.is_open=False
            class Child:
                pid=7654321;returncode=None
                def poll(self):return self.returncode
                def wait(self,timeout):
                    waited.append(timeout);self.returncode=0
                    self_path=root/'sessions'/self.sid
                    self_path.mkdir(parents=True)
                    self_path.joinpath('status.json').write_text(json.dumps(receipt(self.sid)))
                    return 0
            def spawn(command,**kwargs):
                child=Child();child.sid=command[command.index('--session-id')+1]
                (root/'stop.request').touch();return child
            boot_watch.run_watch('COM20',root,serial_factory=Port,popen=spawn)
            self.assertEqual(waited,[250])
            self.assertTrue(json.loads((root/'watch.json').read_text())['cleanup_confirmed'])

    def test_blocking_close_is_bounded_and_refuses_handoff_without_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);release=threading.Event();finished=threading.Event();calls=[]
            class Port:
                is_open=False
                def open(self):self.is_open=True;calls.append('open')
                def read(self,size):return b'UPLINK_WIRED_REQUEST id=1\n'
                def cancel_write(self):calls.append('cancel-write')
                def cancel_read(self):calls.append('cancel-read')
                def close(self):
                    calls.append('close');release.wait(2);self.is_open=False;finished.set()
            original=start.close_serial_transport
            def short_close(port,**kwargs):return original(port,timeout=.04,**kwargs)
            try:
                began=time.monotonic()
                with patch.object(start,'close_serial_transport',side_effect=short_close), \
                     patch.object(boot_watch.subprocess,'Popen',side_effect=AssertionError('spawn forbidden')):
                    with self.assertRaisesRegex(RuntimeError,'release'):
                        boot_watch.run_watch('COM20',root,serial_factory=Port)
                self.assertLess(time.monotonic()-began,.7)
                state=json.loads((root/'watch.json').read_text())
                self.assertEqual(state['phase'],'failed');self.assertFalse(state['cleanup_confirmed'])
                self.assertTrue(state['serial_cleanup_timeout']);self.assertEqual(calls.count('close'),1)
                self.assertEqual(calls[:3],['open','cancel-write','cancel-read'])
            finally:release.set();self.assertTrue(finished.wait(1))
            self.assertFalse(json.loads((root/'watch.json').read_text())['serial_close_confirmed'])

    def test_read_exception_records_failed_and_still_releases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);closed=[]
            class Port:
                is_open=False
                def open(self):self.is_open=True
                def read(self,size):raise RuntimeError('synthetic read failure')
                def close(self):self.is_open=False;closed.append(1)
            with self.assertRaisesRegex(RuntimeError,'synthetic read'):
                boot_watch.run_watch('COM20',root,serial_factory=Port,
                    popen=lambda *a,**k:(_ for _ in ()).throw(AssertionError('spawn forbidden')))
            state=json.loads((root/'watch.json').read_text())
            self.assertEqual(closed,[1]);self.assertEqual(state['phase'],'failed')
            self.assertFalse(state['cleanup_confirmed']);self.assertTrue(state['serial_close_confirmed'])

    def test_dead_watcher_or_legacy_stopped_status_never_claims_cleanup(self):
        for phase in ('waiting-choice','stopped','failed'):
            with self.subTest(phase=phase),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);(root/'watch.json').write_text(json.dumps({'pid':99,'phase':phase}))
                with patch('launch.process_running',return_value=False),self.assertRaises(RuntimeError):
                    boot_watch.request_stop(root,wait=.01)


if __name__=='__main__':unittest.main()

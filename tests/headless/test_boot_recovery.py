"""Exact failed-session recovery, without real COM, processes or network."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools/pc_link'))
import boot_watch
import launch
import start


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.sid='a'*32
        self.watch=dict(schema=1,pid=91,port='COM20',phase='failed',serial_open=False,
                        cleanup_confirmed=False,serial_close_confirmed=True,
                        serial_cleanup_timeout=False,attach_session_id=self.sid,attach_pid=92)
        self.child=dict(session_id=self.sid,forwarder_pid=92,port='COM20',boot_attach=True,
                        phase='failed',serial_close_confirmed=True,serial_receiver_stopped=True,
                        serial_cleanup_timeout=False,gateway_cleanup_confirmed=True,
                        board_finished_marker_seen=False,board_exit_rc=None)
        self.write()
        self.calls=[]
        outer=self
        class Port:
            def open(self):outer.calls.append('open')
            def close(self):outer.calls.append('close')
        self.port=Port()
        self.proof={'console_challenge_confirmed':True,'board_ppp_stopped':True}
        self.running=patch.object(launch,'process_running',return_value=False)
        self.running.start();self.addCleanup(self.running.stop)

    def write(self):
        (self.root/'watch.json').write_text(json.dumps(self.watch),encoding='utf-8')
        folder=self.root/'sessions'/self.sid
        folder.mkdir(parents=True,exist_ok=True)
        (folder/'status.json').write_text(json.dumps(self.child),encoding='utf-8')

    def recover(self,**kw):
        with contextlib.redirect_stdout(io.StringIO()):
            return boot_watch.recover_watch('COM20',self.root,serial_factory=lambda:self.port,
                                           probe=kw.get('probe',lambda p:self.proof))

    def test_fresh_console_recovers_without_rewriting_original_failed_receipt(self):
        original=(self.root/'sessions'/self.sid/'status.json').read_bytes()
        self.assertEqual(self.recover(),0)
        self.assertEqual(self.calls,['open','close'])
        self.assertFalse(self.port.dtr);self.assertFalse(self.port.rts)
        state=json.loads((self.root/'watch.json').read_text())
        self.assertEqual(state['phase'],'stopped');self.assertTrue(state['cleanup_confirmed'])
        evidence=json.loads((self.root/state['recovery_receipt']).read_text())
        self.assertTrue(evidence['board_ppp_stopped'])
        self.assertEqual(evidence['previous_session_id'],self.sid)
        self.assertEqual((self.root/'sessions'/self.sid/'status.json').read_bytes(),original)

    def test_local_cleanup_or_child_identity_failures_never_open_com(self):
        for key,value in [('forwarder_pid',93),('session_id','b'*32),('port','COM21'),
                          ('boot_attach',False),('phase','connected'),
                          ('serial_close_confirmed',False),('serial_receiver_stopped',False),
                          ('serial_cleanup_timeout',True),('gateway_cleanup_confirmed',False)]:
            with self.subTest(key=key):
                old=self.child[key];self.child[key]=value;self.write()
                with self.assertRaises(RuntimeError):self.recover()
                self.child[key]=old
                self.assertEqual(self.calls,[])
        for key,value in [('phase','waiting-choice'),('port','COM21'),
                          ('serial_close_confirmed',False),('serial_cleanup_timeout',True),
                          ('attach_session_id','../../unsafe')]:
            with self.subTest(key=key):
                old=self.watch[key];self.watch[key]=value;self.write()
                with self.assertRaises(RuntimeError):self.recover()
                self.watch[key]=old
                self.assertEqual(self.calls,[])

    def test_live_or_unknown_child_and_another_active_session_are_rejected(self):
        for alive in (True,None):
            with patch.object(launch,'process_running',return_value=alive):
                with self.assertRaisesRegex(RuntimeError,'has not exited'):self.recover()
        with patch.object(launch,'active_sessions',return_value=[self.root/'other']):
            with self.assertRaisesRegex(RuntimeError,'still active'):self.recover()
        self.assertEqual(self.calls,[])

    def test_probe_error_or_incomplete_proof_preserves_failure_and_closes_port(self):
        original=json.loads((self.root/'watch.json').read_text())
        for probe in (lambda p: {'board_ppp_stopped':True},
                      lambda p: (_ for _ in ()).throw(RuntimeError('no NSH'))):
            with self.assertRaises(RuntimeError):self.recover(probe=probe)
            self.assertEqual(json.loads((self.root/'watch.json').read_text()),original)
            self.assertEqual(self.calls[-2:],['open','close'])
        self.assertEqual(list(self.root.glob('recovery-*.json')),[])

    def test_port_close_timeout_never_marks_recovered(self):
        with patch.object(start,'close_serial_transport',return_value={
                'serial_close_confirmed':False,'serial_cleanup_timeout':True}):
            with self.assertRaisesRegex(RuntimeError,'release'):self.recover()
        self.assertFalse(json.loads((self.root/'watch.json').read_text())['cleanup_confirmed'])
        self.assertFalse(json.loads((self.root/'watch.json').read_text())['serial_close_confirmed'])
        with self.assertRaisesRegex(RuntimeError,'serial release'):self.recover()
        self.assertEqual(self.calls,['open'])

    def test_probe_and_close_both_fail_block_any_later_reopen(self):
        def bad_probe(port):raise RuntimeError('synthetic private console failure')
        with patch.object(start,'close_serial_transport',return_value={
                'serial_close_confirmed':False,'serial_cleanup_timeout':True}):
            with self.assertRaisesRegex(RuntimeError,'serial release'):self.recover(probe=bad_probe)
        with self.assertRaisesRegex(RuntimeError,'serial release'):self.recover()
        self.assertEqual(self.calls,['open'])

    def test_os_lock_excludes_running_watcher_even_with_a_failed_snapshot(self):
        with boot_watch.watcher_lock(self.root):
            with self.assertRaisesRegex(RuntimeError,'Another boot watcher'):self.recover()
        self.assertEqual(self.calls,[])

    def test_probe_requires_fresh_echo_and_explicit_stopped_ppp(self):
        class Nsh:
            def __init__(self,port):pass
            def run(self,command):
                if command.startswith('echo '):return command[5:]+'\r\n'
                return 'UPLINK_BOOT active=1 selected=1 attempts=1 ppp_running=0 last_rc=0\r\n'
        port=SimpleNamespace(write=lambda data:None)
        with patch.dict(sys.modules,{'configure_uplink':SimpleNamespace(Nsh=Nsh)}):
            self.assertEqual(boot_watch.recovery_console_probe(port),self.proof)
        for response in ('UPLINK_BOOT active=1 selected=1 attempts=1 ppp_running=1 last_rc=0\n',
                         'quoted UPLINK_BOOT active=1 selected=1 attempts=1 ppp_running=0 last_rc=0\n',
                         'old ap> prompt\n'):
            class BadNsh(Nsh):
                def run(self,command):
                    return super().run(command) if command.startswith('echo ') else response
            with patch.dict(sys.modules,{'configure_uplink':SimpleNamespace(Nsh=BadNsh)}):
                with self.assertRaises(RuntimeError):boot_watch.recovery_console_probe(port)


if __name__=='__main__':unittest.main()

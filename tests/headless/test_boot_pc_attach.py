"""Synthetic-only board selection, safe attachment and serial handoff tests."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HERE=Path(__file__).resolve().parents[2]/'tools/pc_link'
sys.path.insert(0,str(HERE))
import boot_watch
import start
import gateway


def lcp_frame(valid=True):
    payload=b'\xff\x03\xc0\x21\x01\x17\x00\x0a\x05\x06\x12\x34\x56\x78'
    crc=boot_watch.ppp_fcs(payload)^0xffff
    raw=payload+crc.to_bytes(2,'little')
    if not valid:raw=raw[:-1]+bytes([raw[-1]^1])
    wire=bytearray(b'\x7e')
    for byte in raw:
        if byte<32 or byte in (0x7d,0x7e):wire.extend((0x7d,byte^0x20))
        else:wire.append(byte)
    wire.append(0x7e)
    return raw,bytes(wire)


class SelectionTests(unittest.TestCase):
    def test_complete_wired_line_can_be_split_anywhere(self):
        marker=b'UPLINK_WIRED_REQUEST id=123\r\n'
        for at in range(len(marker)):
            parser=boot_watch.BoardSelectionParser()
            first=parser.feed(marker[:at]);last=parser.feed(marker[at:])
            self.assertEqual(first+last,[{'kind':'wired-request','request_line':'UPLINK_WIRED_REQUEST id=123'}])

    def test_nonselection_never_attaches(self):
        for raw in (b'hello\n',b'UPLINK_WIRED_REQUEST id=1',
                    b'quoted UPLINK_WIRED_REQUEST id=1\n',b'UPLINK_WIRED_REQUEST id=0\n',
                    b'UPLINK_WIRED_REQUEST id=4294967296\n',b'UPLINK_WIRED_REQUEST id=-2\n',
                    b'WAKE_DIAG text="UPLINK_WIRED_REQUEST id=1"\n'):
            self.assertEqual(boot_watch.BoardSelectionParser().feed(raw),[])
        self.assertEqual(boot_watch.BoardSelectionParser().feed(b'UPLINK_WIFI_SELECTED\n'),[{'kind':'wifi-selected'}])

    def test_real_lcp_requires_frame_crc_protocol_and_lengths(self):
        raw,wire=lcp_frame()
        self.assertTrue(boot_watch.valid_lcp_request(raw))
        parser=boot_watch.BoardSelectionParser();events=[]
        for byte in wire:events+=parser.feed(bytes([byte]))
        self.assertEqual(events,[{'kind':'ppp-lcp','frame_hex':raw.hex()}])
        self.assertEqual(boot_watch.BoardSelectionParser().feed(lcp_frame(False)[1]),[])
        for bad in (b'\x7e\xc0\x21\x01\x7e',wire[:-1],b'\x7e'+b'x'*1500+b'\x7e'):
            self.assertEqual(boot_watch.BoardSelectionParser().feed(bad),[])

    def test_evidence_is_bound_to_port_freshness_and_exact_selection(self):
        value={'schema':1,'port':'COM20','received_at':100,'kind':'wired-request',
               'request_line':'UPLINK_WIRED_REQUEST id=2'}
        self.assertEqual(boot_watch.validate_evidence(value,'com20',110),value)
        for variant,port,now in ((value,'COM3',110),(value,'COM20',116),(value,'COM20',99),
            (dict(value,request_line='UPLINK_WIFI_SELECTED'),'COM20',110),
            (dict(value,port=20),'COM20',110),(dict(value,received_at=True),'COM20',110),
            (dict(value,schema=True),'COM20',110)):
            with self.assertRaises(RuntimeError):boot_watch.validate_evidence(variant,port,now)
        raw,_=lcp_frame()
        self.assertTrue(boot_watch.validate_evidence(dict(value,kind='ppp-lcp',frame_hex=raw.hex()),'COM20',100))

    def test_file_evidence_is_bounded(self):
        with self.assertRaises(RuntimeError):boot_watch.read_evidence(None,'COM20')
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'evidence.json';path.write_bytes(b' '*4097)
            with self.assertRaises(RuntimeError):boot_watch.read_evidence(path,'COM20')


class AttachTests(unittest.TestCase):
    def test_persistent_duration_is_exclusive_to_boot_attach(self):
        self.assertFalse(start.valid_session_duration(0,False))
        self.assertTrue(start.valid_session_duration(0,True))
        for duration in (-1,1,59,3601,True):
            self.assertFalse(start.valid_session_duration(duration,False))
            self.assertFalse(start.valid_session_duration(duration,True))
        for duration in (60,3600):
            self.assertTrue(start.valid_session_duration(duration,False))
            self.assertTrue(start.valid_session_duration(duration,True))
        with self.assertRaises(ValueError):start.gateway_config(0,False,'synthetic-local-test-token')
        finite=start.gateway_config(3600,False,'synthetic-local-test-token')
        self.assertNotIn('persistent',finite);self.assertNotIn('boot_attach',finite)
        persistent=start.gateway_config(0,True,'synthetic-local-test-token')
        self.assertIs(persistent['persistent'],True);self.assertIs(persistent['boot_attach'],True)
        self.assertEqual(persistent['seconds'],0)
        self.assertTrue(start.session_within_limit(0,86400))
        self.assertFalse(start.session_within_limit(3600,3600))

    def test_gateway_persistence_has_explicit_policy_and_still_stops_on_ppp_exit(self):
        self.assertEqual(gateway.session_deadline({'seconds':3600},100),4000)
        self.assertEqual(gateway.session_deadline({'seconds':0},100),400)
        self.assertEqual(gateway.session_deadline({'seconds':0,'persistent':'true'},100),400)
        persistent={'seconds':0,'boot_attach':True,'persistent':True}
        self.assertIsNone(gateway.session_deadline(persistent,100))
        for bad in ({'seconds':0,'persistent':True},dict(persistent,boot_attach=False),
                    dict(persistent,seconds=60),dict(persistent,seconds=False)):
            with self.assertRaises(ValueError):gateway.session_deadline(bad,100)
        running=SimpleNamespace(poll=lambda:None);ended=SimpleNamespace(poll=lambda:0)
        self.assertTrue(gateway.gateway_should_run(None,running,10**9))
        self.assertFalse(gateway.gateway_should_run(None,ended,100))
        self.assertFalse(gateway.gateway_should_run(400,running,400))

    def test_persistent_gateway_peer_disconnect_runs_normal_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);config=root/'private.json'
            config.write_text(json.dumps({'port':19654,'token':'synthetic',
                'seconds':0,'boot_attach':True,'persistent':True}))
            actions=[]
            class Forward:
                value='0'
                def read_text(self):return self.value
                def write_text(self,value):self.value=value.strip();actions.append(('forward',self.value))
            forward=Forward()
            class Peer:
                auth=bytearray(b'synthetic\n')
                def settimeout(self,*args):pass
                def setsockopt(self,*args):pass
                def recv(self,size):return bytes([self.auth.pop(0)]) if self.auth else b''
                def close(self):actions.append('peer-close')
            peer=Peer()
            class Server:
                def setsockopt(self,*args):pass
                def settimeout(self,*args):pass
                def bind(self,*args):actions.append('bind-fake')
                def listen(self,*args):pass
                def accept(self):return peer,('127.0.0.1',0)
                def close(self):actions.append('server-close')
            class Process:
                ended=False
                stdin=SimpleNamespace(fileno=lambda:12)
                stdout=SimpleNamespace(fileno=lambda:13)
                def poll(self):return 0 if self.ended else None
                def terminate(self):self.ended=True;actions.append('terminate-owned-pppd')
                def wait(self,*args):return 0
                def kill(self):raise AssertionError('normal mock process should not require a kill')
            process=Process()
            class Feed:
                def tick(self):actions.append('feed-tick')
                def close(self):actions.append('feed-close')
            actual_path=Path
            def paths(value):return forward if str(value)=='/proc/sys/net/ipv4/ip_forward' else actual_path(value)
            def rule(command,**kwargs):actions.append(('rule',tuple(command)));return SimpleNamespace(returncode=0)
            output=io.StringIO()
            with patch.object(gateway,'Path',side_effect=paths), \
                 patch.object(gateway.signal,'signal'), \
                 patch.object(gateway.socket,'socket',return_value=Server()), \
                 patch.object(gateway,'FeedCompanion',Feed), \
                 patch.object(gateway.subprocess,'check_output',return_value=b'[{"dev":"synthetic0"}]'), \
                 patch.object(gateway.subprocess,'run',side_effect=rule), \
                 patch.object(gateway.subprocess,'Popen',return_value=process), \
                 patch.object(gateway.select,'select',return_value=([peer],[],[])), \
                 contextlib.redirect_stdout(output):
                gateway.run_gateway(config)
            self.assertIn('PPP_SERVER_CLEANED',output.getvalue())
            self.assertIn('EOFError',output.getvalue())
            self.assertIn('terminate-owned-pppd',actions)
            self.assertIn('feed-close',actions);self.assertIn('peer-close',actions)
            self.assertIn('server-close',actions)
            self.assertEqual(forward.value,'0')
            self.assertFalse((root/'pc_ppp_server.pid').exists())
            rules=[event[1] for event in actions if isinstance(event,tuple) and event[0]=='rule']
            self.assertEqual(sum('-I' in row for row in rules),3)
            self.assertEqual(sum('-D' in row for row in rules),3)

    def test_watcher_excludes_duplicate_instances_without_pid_killing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)
            with boot_watch.watcher_lock(path):
                with self.assertRaises(RuntimeError):boot_watch.watcher_lock(path)
            with boot_watch.watcher_lock(path):pass

    def test_attach_does_not_write_or_read_nsh(self):
        class Port:
            def read(self,*args):raise AssertionError('attach consumed board stream before relay')
            def write(self,*args):raise AssertionError('attach wrote a console command')
        with contextlib.redirect_stdout(io.StringIO()):state=start.prepare_board_transport(Port(),True)
        self.assertEqual(state['clock_sync_state'],'waiting-network')

    def test_board_owned_voice_is_observation_only(self):
        owned=start.BoardOwnedVoice()
        for _ in range(3):owned.start_once()
        owned.wait();self.assertEqual(owned.result['auto_voice_state'],'board-owned')
        actions=[]
        def send(value,timeout):
            actions.append(value['action'])
            if value['action']=='auto-status':return {'rc':0,'auto_voice':{'enabled':False,'running':False}}
            if value['action']=='sync-time':return {'rc':0,'epoch':value['epoch']}
            return {'rc':0}
        health=start.ControlHealth(probe=send);health.enable_voice_status();health.tick();health.wait()
        clock=start.NetworkClock(send=send);clock.update(True);clock.wait()
        self.assertTrue(health.snapshot()['ready'])
        self.assertTrue(health.voice_snapshot()['auto_status_confirmed'])
        self.assertEqual(clock.result['news_clock_sync_state'],'confirmed')
        self.assertEqual(actions,['ping','auto-status','sync-time'])

    def test_read_only_window_uses_last_complete_choice(self):
        class Port:
            def read(self,size):return b'UPLINK_WIRED_REQUEST id=1\nUPLINK_WIFI_SELECTED\n'
            def write(self,*args):raise AssertionError('watcher must only read')
        event=boot_watch.listen_window(Port(),boot_watch.BoardSelectionParser(),lambda:False,1)
        self.assertEqual(event,{'kind':'wifi-selected'})

    def test_watch_closes_serial_before_spawn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);order=[]
            class Port:
                is_open=False
                def open(self):self.is_open=True;order.append('open')
                def read(self,size):return b'UPLINK_WIRED_REQUEST id=11\n'
                def close(self):self.is_open=False;order.append('close')
                def write(self,*args):raise AssertionError('watcher wrote to board')
            port=Port();commands=[]
            def spawn(command,**kwargs):
                self.assertFalse(port.is_open);order.append('spawn');commands.append(command)
                session_id=command[command.index('--session-id')+1]
                receipt=root/'sessions'/session_id/'status.json';receipt.parent.mkdir(parents=True)
                receipt.write_text(json.dumps({'session_id':session_id,'forwarder_pid':7654321,
                    'port':'COM20','boot_attach':True,'phase':'stopped',
                    'serial_close_confirmed':True,'serial_receiver_stopped':True,
                    'serial_cleanup_timeout':False,'gateway_cleanup_confirmed':True,
                    'board_finished_marker_seen':True,'board_exit_line_complete':True,
                    'board_cleanup_confirmed':True,'board_exit_rc':0}))
                (root/'stop.request').touch()
                return SimpleNamespace(pid=7654321,returncode=0,poll=lambda:0)
            boot_watch.run_watch('COM20',root,serial_factory=lambda:port,popen=spawn)
            self.assertEqual(order,['open','close','spawn'])
            command=commands[0]
            self.assertIn('--boot-attach',command);self.assertIn('--boot-evidence',command)
            self.assertIn('--external-stop-request',command)
            self.assertEqual(command[command.index('--duration')+1],'0')
            self.assertNotIn('--manual-voice',command)
            evidence=json.loads((root/'boot_evidence.json').read_text())
            self.assertEqual(evidence['request_line'],'UPLINK_WIRED_REQUEST id=11')
            status=json.loads((root/'watch.json').read_text());self.assertEqual(status['phase'],'stopped')

    def test_wireless_never_starts_a_forwarder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            class Port:
                is_open=False
                def open(self):self.is_open=True
                def read(self,size):return b'UPLINK_WIFI_SELECTED\n'
                def close(self):self.is_open=False;(root/'stop.request').touch()
                def write(self,*args):raise AssertionError('watcher wrote to board')
            def forbidden(*args,**kwargs):raise AssertionError('wireless must not start PPP')
            boot_watch.run_watch('COM20',root,serial_factory=Port,popen=forbidden)

    def test_open_discards_old_driver_backlog_before_accepting_a_choice(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            class Port:
                is_open=False
                incoming=b'UPLINK_WIRED_REQUEST id=99\n'
                def open(self):self.is_open=True
                def reset_input_buffer(self):self.incoming=b'UPLINK_WIFI_SELECTED\n'
                def read(self,size):return self.incoming
                def close(self):self.is_open=False;(root/'stop.request').touch()
            def forbidden(*args,**kwargs):raise AssertionError('old marker backlog triggered PPP')
            boot_watch.run_watch('COM20',root,serial_factory=Port,popen=forbidden)


if __name__=='__main__':unittest.main()

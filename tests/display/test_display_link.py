"""Display link protocol/state tests; every serial/RPC dependency is synthetic."""
from pathlib import Path
import importlib.util
import contextlib
import io
import json
import sys
import tempfile
import threading
import time
from types import ModuleType,SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location('display_link_under_test',ROOT/'tools/display_link.py')
display=importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name]=display
SPEC.loader.exec_module(display)


class Clock:
    def __init__(self):self.value=0.0
    def __call__(self):return self.value
    def advance(self,value=.05):self.value+=value


class FakeSerial:
    """Respond to ASCII line requests without importing/opening pyserial."""
    def __init__(self,clock=None,responder=None):
        self.clock=clock or Clock();self.responder=responder
        self.writes=[];self.buffer=bytearray();self.closed=False;self.timeout=.05
    @property
    def in_waiting(self):return len(self.buffer)
    def write(self,data):
        self.writes.append(bytes(data));request=json.loads(data)
        value=self.responder(request) if self.responder else self.ack(request)
        if value is not None:
            if isinstance(value,dict):value=json.dumps(value,separators=(',',':')).encode()+b'\n'
            self.buffer.extend(value)
        return len(data)
    @staticmethod
    def ack(request):
        return {'type':'zhaoxi.display','v':1,'id':request['id'],'ok':True,'cmd':request['cmd'],
                'state':request.get('state','clock'),'active':request.get('state','clock')!='clock','frame':1,'uptime_ms':100,
                'device':'EasyMatrix','width':32,'height':8}
    def read(self,count=1):
        if not self.buffer:self.clock.advance();return b''
        value=bytes(self.buffer[:count]);del self.buffer[:count];return value
    def readline(self,size=None):
        if not self.buffer:self.clock.advance();return b''
        at=self.buffer.find(b'\n')
        count=at+1 if at>=0 else len(self.buffer)
        return self.read(min(count,size) if size is not None else count)
    def read_until(self,expected=b'\n',size=None):return self.readline()
    def flush(self):pass
    def reset_input_buffer(self):self.buffer.clear()
    def close(self):self.closed=True


def port(name,vid,pid):
    return SimpleNamespace(device=name,vid=vid,pid=pid,description='synthetic port',serial_number='synthetic')


def production_esp_ack(request):
    """All fields emitted by extension/EasyMatrix/emotion_display.cpp/respond."""
    state=request.get('state','clock')
    return {'type':'zhaoxi.display','v':1,'id':request['id'],'ok':True,'cmd':request['cmd'],
            'device':'EasyMatrix','fw':'zhaoxi-display-1.0.0','width':32,'height':8,
            'state':state,'active':state!='clock','frame':123456,'uptime_ms':12345678,
            'base_page':1,'ttl_ms':10000 if state!='clock' else 0}


LEGACY_MENU = '可用命令：config = 重启进入配网模式 ; icons = 逐个演示7张时刻图案'
CURRENT_MENU = LEGACY_MENU + ' ; JSON ping/status/emotion'


class PortTests(unittest.TestCase):
    def setUp(self):
        self.good=port('COM4',0x1a86,0x55d3)
        self.bes=port('COM3',0x0403,0x6001)
        self.other=port('COM5',0x1a86,0x7523)
    def test_only_ch343_is_selected(self):
        chosen=display.choose_port([self.bes,self.other,self.good])
        self.assertEqual(getattr(chosen,'device',chosen),'COM4')
    def test_explicit_bes_or_wrong_usb_device_is_rejected(self):
        for name in ('COM3','COM5','COM99'):
            with self.subTest(name=name),self.assertRaises((ValueError,RuntimeError)):
                display.choose_port([self.bes,self.other,self.good],name)
    def test_no_device_or_multiple_devices_requires_explicit_selection(self):
        for ports in ([],[self.good,port('COM6',0x1a86,0x55d3)]):
            with self.assertRaises((ValueError,RuntimeError)):display.choose_port(ports)
        chosen=display.choose_port([self.good,port('COM6',0x1a86,0x55d3)],'COM6')
        self.assertEqual(getattr(chosen,'device',chosen),'COM6')


class ProtocolTests(unittest.TestCase):
    def test_complete_production_esp_ack_exceeding_request_limit_is_accepted(self):
        clock=Clock();responses=[]
        def respond(request):
            ack=production_esp_ack(request)
            wire=json.dumps(ack,separators=(',',':')).encode()+b'\n';responses.append(wire)
            self.assertGreater(len(wire),192);self.assertLessEqual(len(wire),512)
            return wire
        serial=FakeSerial(clock,respond);client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        handshake=client.handshake();emotion=client.request('emotion','listening')
        self.assertEqual(handshake['fw'],'zhaoxi-display-1.0.0')
        self.assertEqual(emotion['state'],'listening');self.assertEqual(emotion['ttl_ms'],10000)
        self.assertEqual(emotion['base_page'],1);self.assertEqual(len(responses),3)
        self.assertEqual(display.MAX_LINE,192);self.assertEqual(display.ACK_MAX_LINE,512)
        self.assertTrue(all(len(wire)<=192 for wire in serial.writes))

    def test_ack_boundary_allows_512_but_rejects_513_bytes(self):
        for size in (512,513):
            with self.subTest(size=size):
                clock=Clock()
                def respond(request):
                    ack=production_esp_ack(request);ack['padding']=''
                    initial=len(json.dumps(ack,separators=(',',':')).encode())+1
                    ack['padding']='x'*(size-initial)
                    wire=json.dumps(ack,separators=(',',':')).encode()+b'\n'
                    self.assertEqual(len(wire),size);return wire
                serial=FakeSerial(clock,respond);client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
                if size==512:self.assertTrue(client.request('ping')['ok'])
                else:
                    with self.assertRaises(display.ProtocolError):client.request('ping')
                self.assertEqual(len(serial.writes),1)

    def test_handshake_uses_ping_and_status(self):
        clock=Clock();serial=FakeSerial(clock)
        client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        client.handshake()
        self.assertEqual([json.loads(value)['cmd'] for value in serial.writes],['ping','status'])
        for line in serial.writes:
            self.assertLessEqual(len(line),192);self.assertTrue(line.endswith(b'\n'))
            self.assertTrue(line.isascii())
            body=json.loads(line);self.assertIn('id',body);self.assertIn('cmd',body)
    def test_missing_response_times_out_without_unbounded_retry(self):
        clock=Clock();serial=FakeSerial(clock,lambda request:None)
        with self.assertRaises((TimeoutError,ValueError,RuntimeError)):
            display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('ping')
        self.assertEqual(len(serial.writes),1)

    def test_correlated_ack_schema_is_strict(self):
        invalid=[('type','wrong'),('v',2),('v',True),('ok',False),('ok',1),('cmd','status'),
                 ('state','unrecognized'),('active',1),('frame',True),('frame',-1),
                 ('uptime_ms',True),('uptime_ms',-1),('device','OtherDevice'),
                 ('width',64),('height',16),('width',32.0),('height',8.0)]
        for key,value in invalid:
            with self.subTest(key=key,value=value):
                clock=Clock()
                def respond(request):
                    ack=FakeSerial.ack(request);ack[key]=value;return ack
                serial=FakeSerial(clock,respond)
                with self.assertRaises(display.ProtocolError):
                    display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('ping')
                self.assertEqual(len(serial.writes),1)

    def test_missing_ack_fields_cannot_confirm_a_command(self):
        for field in ('type','v','ok','cmd','state','active','frame','uptime_ms','device','width','height'):
            with self.subTest(field=field):
                clock=Clock()
                def respond(request):
                    ack=FakeSerial.ack(request);ack.pop(field);return ack
                serial=FakeSerial(clock,respond)
                with self.assertRaises(display.ProtocolError):
                    display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('ping')

    def test_stale_id_is_skipped_until_matching_ack(self):
        clock=Clock()
        def respond(request):
            stale=FakeSerial.ack(request);stale['id']=request['id']+100
            correct=FakeSerial.ack(request)
            return (json.dumps(stale)+'\n'+json.dumps(correct)+'\n').encode()
        serial=FakeSerial(clock,respond)
        client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        first=client.request('ping');second=client.request('status')
        self.assertNotEqual(json.loads(serial.writes[0])['id'],json.loads(serial.writes[1])['id'])
        self.assertEqual(first['cmd'],'ping');self.assertEqual(second['cmd'],'status')

    def test_unrelated_and_nonprotocol_lines_do_not_confirm_a_command(self):
        clock=Clock()
        def respond(request):
            ack=FakeSerial.ack(request)
            return b'boot log\n{bad json\n'+json.dumps(ack).encode()+b'\n'
        serial=FakeSerial(clock,respond)
        ack=display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('ping')
        self.assertEqual(ack['ok'],True)

    def test_emotion_has_fixed_lease_and_matching_ack_state(self):
        clock=Clock();serial=FakeSerial(clock)
        client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        for state in display.STATES:
            ack=client.request('emotion',state)
            request=json.loads(serial.writes[-1])
            self.assertEqual(request['state'],state);self.assertEqual(request['ttl_ms'],10000)
            self.assertEqual(ack['state'],state);self.assertLessEqual(len(serial.writes[-1]),192)
        writes=len(serial.writes)
        for command,state,ttl in [('emotion','not_a_state',10000),('emotion','idle',0),
                                  ('emotion','idle',60000),('emotion','idle',10000.0),('emotion','idle',True),
                                  ('ping','idle',10000),('send_text','idle',10000)]:
            with self.assertRaises(ValueError):client.request(command,state,ttl)
        self.assertEqual(len(serial.writes),writes)

    def test_wrong_emotion_is_not_retried(self):
        clock=Clock()
        def respond(request):
            ack=FakeSerial.ack(request);ack['state']='clock';return ack
        serial=FakeSerial(clock,respond)
        with self.assertRaises(display.ProtocolError):
            display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('emotion','thinking')
        self.assertEqual(len(serial.writes),1)

    def test_matching_state_with_wrong_overlay_active_is_rejected(self):
        for state in ('thinking','clock'):
            with self.subTest(state=state):
                clock=Clock()
                def respond(request):
                    ack=FakeSerial.ack(request);ack['active']=state=='clock';return ack
                serial=FakeSerial(clock,respond)
                with self.assertRaises(display.ProtocolError):
                    display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('emotion',state)
                self.assertEqual(len(serial.writes),1)

    def test_boolean_id_and_raw_error_data_are_not_accepted_or_echoed(self):
        for field,value in [('id',True),('device','PRIVATE_RAW_DO_NOT_LOG')]:
            clock=Clock()
            def respond(request):
                ack=FakeSerial.ack(request);ack[field]=value;return ack
            serial=FakeSerial(clock,respond)
            with self.assertRaises(display.ProtocolError) as error:
                display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('ping')
            self.assertNotIn('PRIVATE_RAW_DO_NOT_LOG',str(error.exception))

    def test_oversized_or_unterminated_json_does_not_confirm_a_command(self):
        for response in (b'x'*513+b'\n',b'{"id":1,"ok":true}'):
            clock=Clock();serial=FakeSerial(clock,lambda request:response)
            with self.assertRaises(display.ProtocolError):
                display.DisplayClient(serial,clock=clock,ack_timeout=.2).request('ping')
            self.assertEqual(len(serial.writes),1)

    def test_partial_serial_write_is_not_retried(self):
        clock=Clock()
        class ShortSerial(FakeSerial):
            def write(self,data):super().write(data);return len(data)-1
        serial=ShortSerial(clock)
        with self.assertRaises(display.ProtocolError):display.DisplayClient(serial,clock=clock).request('ping')
        self.assertEqual(len(serial.writes),1)


class BootWaitTests(unittest.TestCase):
    def wait(self, client, clock, seconds=70):
        emitted=[]
        value=display.wait_for_display_ready(client, boot_wait_seconds=seconds,
            clock=clock, sleep=clock.advance,
            emit=lambda *args, **kwargs: emitted.append(args))
        return value, emitted

    def test_ready_immediately_needs_one_ping_status_and_no_wait_message(self):
        clock=Clock();serial=FakeSerial(clock)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
        _, emitted=self.wait(client, clock)
        self.assertEqual([json.loads(value)['cmd'] for value in serial.writes], ['ping','status'])
        self.assertEqual(emitted, []);self.assertEqual(clock.value, 0)

    def test_delayed_boot_retries_only_ping_on_five_second_boundaries(self):
        clock=Clock();attempts=[]
        def respond(request):
            attempts.append((clock(), request['cmd']))
            return FakeSerial.ack(request) if clock() >= 12 else None
        serial=FakeSerial(clock, respond)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
        _, emitted=self.wait(client, clock)
        pings=[at for at, command in attempts if command=='ping']
        self.assertGreaterEqual(len(pings), 3)
        self.assertTrue(all(right-left >= 5-1e-8 for left,right in zip(pings,pings[1:])))
        self.assertEqual([command for _,command in attempts][-2:], ['ping','status'])
        self.assertNotIn('emotion', [command for _,command in attempts])
        self.assertGreaterEqual(clock(), 12);self.assertLessEqual(clock(), 70)
        self.assertEqual(len(emitted), 1)
        client.request('emotion', 'thinking')
        self.assertEqual(json.loads(serial.writes[-1])['state'], 'thinking')

    def test_status_not_ready_restarts_with_ping_before_emotion(self):
        clock=Clock();attempts=[];statuses=[]
        def respond(request):
            attempts.append((clock(), request['cmd']))
            if request['cmd']=='status':
                statuses.append(clock())
                if len(statuses)<3:return None
            return FakeSerial.ack(request)
        serial=FakeSerial(clock, respond)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
        _, emitted=self.wait(client, clock)
        self.assertEqual([command for _,command in attempts], ['ping','status']*3)
        pings=[at for at,command in attempts if command=='ping']
        self.assertTrue(all(right-left >= 5-1e-8 for left,right in zip(pings,pings[1:])))
        self.assertEqual(len(emitted), 1)

    def test_default_boot_deadline_is_bounded_and_does_not_emit_emotions(self):
        self.assertTrue(issubclass(display.NotReadyError, display.ProtocolError))
        clock=Clock();serial=FakeSerial(clock, lambda request:None)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=1.5)
        emitted=[]
        with self.assertRaises(display.NotReadyError):
            display.wait_for_display_ready(client, clock=clock, sleep=clock.advance,
                emit=lambda *args, **kwargs:emitted.append(args))
        self.assertGreaterEqual(clock(), 69.99)
        self.assertLessEqual(clock(), 70.05)
        self.assertTrue(all(json.loads(value)['cmd']=='ping' for value in serial.writes))
        self.assertLessEqual(len(serial.writes), 15)
        self.assertEqual(len(emitted), 1)

    def test_final_ack_wait_uses_only_remaining_boot_budget(self):
        clock=Clock();serial=FakeSerial(clock, lambda request:None)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=3)
        with self.assertRaises(display.NotReadyError):self.wait(client, clock, seconds=9)
        self.assertGreaterEqual(clock(), 8.99);self.assertLessEqual(clock(), 9.05)
        self.assertEqual([json.loads(value)['cmd'] for value in serial.writes], ['ping','ping'])

    def test_request_budget_clamps_and_restores_serial_timeouts(self):
        for outcome in ('ready', 'silent', 'invalid'):
            with self.subTest(outcome=outcome):
                clock=Clock();observed=[]
                def respond(request):
                    value=FakeSerial.ack(request)
                    if outcome=='invalid':value['ok']=False
                    return None if outcome=='silent' else value
                class BudgetSerial(FakeSerial):
                    def write(self, data):
                        observed.append(('write', self.write_timeout))
                        return super().write(data)
                    def readline(self, size=None):
                        observed.append(('read', self.timeout))
                        if not self.buffer:
                            self.clock.advance(min(.02, self.timeout))
                            return b''
                        return super().readline(size)
                serial=BudgetSerial(clock, respond);serial.timeout=2;serial.write_timeout=3
                client=display.DisplayClient(serial, clock=clock)
                if outcome=='ready':self.assertTrue(client.request('ping', timeout=.1)['ok'])
                else:
                    with self.assertRaises(display.ProtocolError):client.request('ping', timeout=.1)
                self.assertTrue(observed)
                self.assertTrue(all(0 < budget <= .1+1e-8 for _,budget in observed))
                self.assertEqual(serial.timeout, 2);self.assertEqual(serial.write_timeout, 3)
                self.assertLessEqual(clock(), .1+1e-8)

    def test_bad_correlated_ack_is_not_retried_as_boot_delay(self):
        for changes in ({'v':2}, {'ok':False}, {'width':64}):
            with self.subTest(changes=changes):
                clock=Clock()
                def respond(request):
                    value=FakeSerial.ack(request);value.update(changes);return value
                serial=FakeSerial(clock, respond)
                client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
                with self.assertRaises(display.ProtocolError) as raised:self.wait(client, clock)
                self.assertNotIsInstance(raised.exception, display.NotReadyError)
                self.assertEqual([json.loads(value)['cmd'] for value in serial.writes], ['ping'])
                self.assertEqual(clock(), 0)

    def test_old_menu_fails_immediately_while_current_menu_can_precede_json(self):
        clock=Clock()
        old=(LEGACY_MENU+' PRIVATE_OLD_MENU_SENTINEL\n').encode()
        serial=FakeSerial(clock, lambda request:old)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
        with self.assertRaises(display.ProtocolError) as raised:self.wait(client, clock)
        self.assertNotIsInstance(raised.exception, display.NotReadyError)
        self.assertEqual(len(serial.writes), 1);self.assertEqual(clock(), 0)
        self.assertNotIn('PRIVATE_OLD_MENU_SENTINEL', str(raised.exception))
        self.assertNotIn('可用命令', str(raised.exception))

        clock=Clock()
        def current(request):
            return ((CURRENT_MENU+'\n').encode()+
                    json.dumps(FakeSerial.ack(request)).encode()+b'\n')
        serial=FakeSerial(clock, current)
        client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
        _, emitted=self.wait(client, clock)
        self.assertEqual([json.loads(value)['cmd'] for value in serial.writes], ['ping','status'])
        self.assertEqual(emitted, [])


class TraceTests(unittest.TestCase):
    def test_note_and_drink_receipts_emit_success_or_error_from_rc_only(self):
        for label, operations in (('NOTE_RESULT', ('add', 'list')),
                                  ('DRINK_RESULT', ('start', 'status', 'stop'))):
            for operation in operations:
                for rc, feedback in ((0, 'success'), (-5, 'error'), (12, 'error')):
                    with self.subTest(label=label, operation=operation, rc=rc):
                        raw=(f'[01/11/26 14:56:20.243000] [67] [ap] '
                             f'{label} op={operation} rc={rc} '
                             'path=/private/DO_NOT_EXPORT_NOTE.md private body\n').encode()
                        self.assertEqual(display.parse_trace(raw), [('result', feedback)])

    def test_note_and_drink_receipts_without_rc_have_no_feedback(self):
        for label in ('NOTE_RESULT', 'DRINK_RESULT'):
            with self.subTest(label=label):
                raw=(f'{label} op=add path=/private/DO_NOT_EXPORT.md failed private body\n'
                     f'{label} op=status rc=not_available private body\n'
                     f'{label} op=stop rc= private body\n'
                     f'private message quotes {label} op=add rc=0\n').encode()
                self.assertEqual(display.parse_trace(raw), [])

    def test_note_and_drink_private_body_and_path_never_reach_display_wire(self):
        for label, operation, rc, feedback in (('NOTE_RESULT', 'add', 0, 'success'),
                                               ('DRINK_RESULT', 'start', -5, 'error')):
            with self.subTest(label=label):
                private='DO_NOT_EXPORT_PRIVATE_NOTE_保密正文'
                raw=(f'{label} op={operation} rc={rc} '
                     f'path=/private/{private}.md body="{private}"\n').encode()
                events=display.parse_trace(raw)
                self.assertEqual(events, [('result', feedback)])
                self.assertNotIn(private, repr(events))
                self.assertNotIn('/private/', repr(events))
                mapper=display.StateMapper();mapper.consume(events, 0)
                clock=Clock();serial=FakeSerial(clock)
                client=display.DisplayClient(serial, clock=clock, ack_timeout=.2)
                client.request('emotion', mapper.desired({}, .1))
                self.assertEqual(json.loads(serial.writes[0])['state'], feedback)
                self.assertNotIn(private.encode(), b''.join(serial.writes))
                self.assertNotIn(b'/private/', b''.join(serial.writes))
                self.assertNotIn(label.encode(), b''.join(serial.writes))
                self.assertEqual(set(json.loads(serial.writes[0])),
                                 {'id', 'cmd', 'state', 'ttl_ms'})

    def test_parser_emits_metadata_without_body(self):
        secret='DO_NOT_EXPORT_PRIVATE_TEXT'
        raw=(f'[trace:abcdef12] BEGIN chat=private chan=voice {secret}\n'
             f'[trace:abcdef12] END status=ok iters=1 {secret}\n'
             f'TODO_RESULT op=add rc=0 id=3 path=/private/{secret}\n'
             f'TO_DO_RESULT op=done rc=-2 id=9 {secret}\n'
             f'[Reminder]: {secret}\n'
             f'[voice] speak: "{secret}"\n').encode()
        events=display.parse_trace(raw)
        self.assertEqual(events,[('begin','abcdef12'),('end',('abcdef12','ok')),
                                 ('result','success'),('result','error'),('reminder','success')])
        self.assertNotIn(secret,repr(events));self.assertNotIn('/private',repr(events))

    def test_bad_trace_identifiers_and_unrelated_lines_are_ignored(self):
        raw=(b'[trace:abc] BEGIN\n[trace:ghijklmn] END status=ok\n'
             b'ordinary private body\nTTS PCM bytes rx=100\n')
        self.assertEqual(display.parse_trace(raw),[])

    def test_serial_events_ignore_history_and_read_new_records_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'serial.bin'
            path.write_bytes(b'[trace:11111111] BEGIN old private text\n')
            reader=display.SerialEvents(path)
            self.assertEqual(reader.read_new(),[])
            with path.open('ab') as stream:stream.write(b'[trace:22222222] BEGIN new private text\n')
            self.assertEqual(reader.read_new(),[('begin','22222222')])
            self.assertEqual(reader.read_new(),[])

    def test_prefixed_traces_and_fragmented_records_are_supported(self):
        raw=(b'[01/11/26 14:56:20.243000] [67] [ap] [trace:abcdef02468ace00] BEGIN chat=private\n'
             b'[01/11/26 14:56:22.276000] [67] [ap] [trace:abcdef02468ace00] END status=timeout elapsed=2s\n'
             b'REMINDER_DELIVERY mode=speech rc=0 speech_played=1\n')
        self.assertEqual(display.parse_trace(raw),[('begin','abcdef02468ace00'),
                         ('end',('abcdef02468ace00','timeout')),('result','success')])
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'serial.bin';path.write_bytes(b'')
            reader=display.SerialEvents(path)
            with path.open('ab') as stream:stream.write(b'[trace:12345678] BE')
            self.assertEqual(reader.read_new(),[])
            with path.open('ab') as stream:stream.write(b'GIN private body\n')
            self.assertEqual(reader.read_new(),[('begin','12345678')])
            self.assertEqual(reader.read_new(),[])


class MappingTests(unittest.TestCase):
    def test_note_and_drink_receipt_feedback_is_a_three_second_overlay(self):
        sleeping={'wake_enabled':True, 'awake':False}
        for label in ('NOTE_RESULT', 'DRINK_RESULT'):
            for rc, feedback in ((0, 'success'), (-7, 'error')):
                with self.subTest(label=label, rc=rc):
                    mapper=display.StateMapper()
                    events=display.parse_trace(f'{label} op=status rc={rc}\n'.encode())
                    mapper.consume(events, 10)
                    self.assertEqual(mapper.desired(sleeping, 10), feedback)
                    self.assertEqual(mapper.desired(sleeping, 12.99), feedback)
                    self.assertEqual(mapper.desired(sleeping, 13), 'sleep')

    def test_eight_fixed_states_and_output_priority(self):
        self.assertEqual(set(display.STATES),{'idle','listening','thinking','speaking','success','error','sleep','clock'})
        mapper=display.StateMapper();mapper.consume([('result','error')],0)
        self.assertEqual(mapper.desired({'output_playing':True,'output_active':True},.1),'speaking')
        self.assertEqual(mapper.desired({'output_active':True},.1),'thinking')
        self.assertEqual(mapper.desired({'state':'正在播放回答，暂停聆听'},.1),'speaking')
        text_playback={'enabled':False,'running':False,'output_playing':True,
                       'wake_enabled':True,'awake':False}
        self.assertEqual(mapper.desired(text_playback,.1),'speaking')
        self.assertEqual(mapper.desired(dict(text_playback,output_playing=False,output_active=True),.1),'thinking')

    def test_trace_busy_completion_and_overlay_expiry(self):
        mapper=display.StateMapper();mapper.consume([('begin','12345678')],0)
        self.assertEqual(mapper.desired({},1),'thinking')
        mapper.consume([('end',('12345678','ok'))],2)
        self.assertEqual(mapper.desired({},4.99),'success')
        self.assertEqual(mapper.desired({},5.0),'idle')
        for status in ('fail','failed','error','timeout'):
            mapper.consume([('end',('87654321',status))],10)
            self.assertEqual(mapper.desired({},10),'error')
            self.assertEqual(mapper.desired({},13),'idle')

    def test_business_and_trace_feedback_take_priority_over_sleep(self):
        sleeping={'wake_enabled':True,'awake':False}
        mapper=display.StateMapper();self.assertEqual(mapper.desired(sleeping,0),'sleep')
        mapper.consume([('begin','12345678')],1)
        self.assertEqual(mapper.desired(sleeping,2),'thinking')
        mapper.consume([('end',('12345678','ok'))],3)
        self.assertEqual(mapper.desired(sleeping,4),'success')
        self.assertEqual(mapper.desired(sleeping,6),'sleep')
        mapper.consume([('result','error')],10)
        self.assertEqual(mapper.desired(sleeping,11),'error')

    def test_listening_asr_waiting_and_error_metadata(self):
        mapper=display.StateMapper();base={'enabled':True,'running':True}
        for label in ('正在聆听','等待说话','校准环境噪声'):
            self.assertEqual(mapper.desired(dict(base,state=label),0),'listening')
        self.assertEqual(mapper.desired(dict(base,state='other',accepting_speech=True),0),'listening')
        for label in ('正在识别','等待助手回答','正在合成语音，暂停聆听'):
            self.assertEqual(mapper.desired(dict(base,state=label),0),'thinking')
        for label in ('语音异常','本次失败','回答超时'):
            self.assertEqual(mapper.desired(dict(base,state=label),0),'error')
        self.assertEqual(mapper.desired(dict(base,state='other',last_error=-5),0),'error')

    def test_rpc_failure_has_bounded_error_and_session_loss_is_clock(self):
        mapper=display.StateMapper();mapper.consume([('begin','12345678')],0)
        self.assertEqual(mapper.desired({},1,rpc_failed=True),'error')
        self.assertEqual(mapper.desired({},2.99,rpc_failed=True),'error')
        self.assertEqual(mapper.desired({},3,rpc_failed=True),'clock')
        self.assertEqual(mapper.desired({},4,session_ready=False),'clock')
        self.assertEqual(mapper.desired({},5,rpc_failed=True),'error')
        self.assertEqual(mapper.desired({'output_playing':True},5.1,rpc_failed=False),'speaking')
        self.assertEqual(mapper.desired({},5.2,rpc_failed=True),'error')

    def test_stale_trace_does_not_leave_thinking_forever(self):
        mapper=display.StateMapper();mapper.consume([('begin','12345678')],0)
        self.assertEqual(mapper.desired({},119),'thinking')
        self.assertEqual(mapper.desired({},120),'idle')


class PollerTests(unittest.TestCase):
    def test_only_readonly_status_is_polled_without_overlap(self):
        clock=Clock();entered=threading.Event();release=threading.Event();calls=[]
        def request(value,timeout):
            calls.append((value,timeout));entered.set();release.wait(1)
            return {'rc':0,'auto_voice':{'state':'等待说话'}}
        poller=display.ReadOnlyPoller(request,clock)
        try:
            poller.tick(ready=False);self.assertEqual(calls,[])
            poller.tick();self.assertTrue(entered.wait(.5))
            for _ in range(5):clock.advance(1);poller.tick()
            self.assertEqual(calls,[({'action':'auto-status'},2)])
            self.assertEqual(poller.snapshot(),(None,True))
        finally:release.set();poller.thread.join(1)
        metadata,failed=poller.snapshot()
        self.assertFalse(failed);self.assertEqual(metadata['state'],'等待说话')
        clock.advance(7);self.assertEqual(poller.snapshot(),(None,True))

    def test_reset_discards_old_generation_response(self):
        clock=Clock();entered=threading.Event();release=threading.Event()
        def request(value,timeout):
            entered.set();release.wait(1);return {'rc':0,'auto_voice':{'state':'old'}}
        poller=display.ReadOnlyPoller(request,clock)
        try:
            poller.tick();self.assertTrue(entered.wait(.5));poller.reset()
            self.assertEqual(poller.snapshot(),(None,False))
        finally:release.set();poller.thread.join(1)
        self.assertEqual(poller.snapshot(),(None,False))

    def test_rpc_failure_or_invalid_response_is_not_success(self):
        def raises(value,timeout):raise TimeoutError('synthetic RPC timeout')
        for response in ({'rc':-1,'auto_voice':{}},{'rc':0,'auto_voice':'not metadata'},{'rc':0}):
            poller=display.ReadOnlyPoller(lambda value,timeout:response,Clock())
            poller.tick();poller.thread.join(1)
            self.assertEqual(poller.snapshot(),(None,True))
        poller=display.ReadOnlyPoller(raises,Clock());poller.tick();poller.thread.join(1)
        self.assertEqual(poller.snapshot(),(None,True))


class BridgeTests(unittest.TestCase):
    def test_lease_renewal_and_session_loss_emit_only_fixed_states(self):
        clock=Clock();requests=[]
        client=SimpleNamespace(request=lambda *args:requests.append(args))
        bridge=display.DisplayBridge(client,clock=clock,loader=lambda root,explicit:(None,None))
        self.assertEqual(bridge.step(),'clock');self.assertEqual(requests,[('emotion','clock')])
        clock.advance(4.9);bridge.step();self.assertEqual(len(requests),1)
        clock.advance(.1);bridge.step();self.assertEqual(len(requests),2)
        bridge.close();self.assertEqual(requests[-1],('emotion','clock'))

    def test_session_switch_ignores_old_raw_history_and_resets_busy_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            a=Path(temporary)/'a';b=Path(temporary)/'b';a.mkdir();b.mkdir()
            for path in (a,b):(path/'serial_private.bin').write_bytes(b'[trace:11111111] BEGIN stale private body\n')
            current=[a];clock=Clock();requests=[]
            client=SimpleNamespace(request=lambda *args:requests.append(args))
            def loader(root,explicit):return current[0],{'ready':True}
            def rpc(value,timeout):return {'rc':0,'auto_voice':{'enabled':False,'state':'已停止','private':'DO_NOT_EXPORT'}}
            bridge=display.DisplayBridge(client,clock=clock,loader=loader,request=rpc)
            bridge.step();bridge.poller.thread.join(1)
            self.assertEqual(bridge.step(),'idle')
            with (a/'serial_private.bin').open('ab') as stream:stream.write(b'[trace:22222222] BEGIN fresh private body\n')
            self.assertEqual(bridge.step(),'thinking')
            current[0]=b;clock.advance(.1);bridge.step()
            if bridge.poller.thread:bridge.poller.thread.join(1)
            self.assertEqual(bridge.step(),'idle')
            self.assertNotIn('DO_NOT_EXPORT',repr(requests));self.assertNotIn('private',repr(requests))


class SessionTests(unittest.TestCase):
    def test_dead_or_stopped_newer_session_does_not_hide_live_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name,pid,phase in [('20261102_010000',11,'connected'),('20261102_020000',22,'connected'),
                                    ('20261102_030000',11,'stopped')]:
                directory=root/name;directory.mkdir()
                (directory/'status.json').write_text(json.dumps({'forwarder_pid':pid,'phase':phase,'ready':True}))
            with patch.object(sys,'path',[str(ROOT/'tools')]+sys.path):
                directory,status=display.session_snapshot(root,alive=lambda pid:pid==11)
                self.assertEqual(directory,root/'20261102_010000')
                self.assertEqual(status['forwarder_pid'],11)
                self.assertEqual(display.session_snapshot(root,root/'20261102_020000',alive=lambda pid:False),(None,None))

    def test_invalid_and_failed_session_documents_are_skipped(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);directory=root/'20261102_030000';directory.mkdir()
            (directory/'status.json').write_text('{incomplete')
            with patch.object(sys,'path',[str(ROOT/'tools')]+sys.path):
                self.assertEqual(display.session_snapshot(root,alive=lambda pid:True),(None,None))
                (directory/'status.json').write_text(json.dumps({'phase':'failed','forwarder_pid':11}))
                self.assertEqual(display.session_snapshot(root,alive=lambda pid:True),(None,None))


def serial_module_stubs(ports):
    serial=ModuleType('serial');tools=ModuleType('serial.tools');list_ports=ModuleType('serial.tools.list_ports')
    list_ports.comports=lambda:list(ports)
    serial.tools=tools;tools.list_ports=list_ports
    return {'serial':serial,'serial.tools':tools,'serial.tools.list_ports':list_ports}


class CliTests(unittest.TestCase):
    def test_boot_wait_range_is_checked_before_opening_any_serial(self):
        for value in ('-1', '90.1', 'nan', 'inf'):
            with self.subTest(value=value),patch.object(display,'open_display_port') as opened, \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    display.main(['--boot-wait-seconds',value,'--show','idle'])
                self.assertEqual(raised.exception.code, 2)
                opened.assert_not_called()

    def test_boot_wait_endpoints_allow_an_immediately_ready_display(self):
        for value in ('0','90'):
            with self.subTest(value=value):
                clock=Clock();serial=FakeSerial(clock)
                client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
                modules=serial_module_stubs([port('COM4',0x1a86,0x55d3)])
                with patch.dict(sys.modules,modules),patch.object(display,'open_display_port',return_value=serial), \
                     patch.object(display,'DisplayClient',return_value=client), \
                     patch.object(display.time,'monotonic',clock),patch.object(display.time,'sleep',clock.advance), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(display.main(['--boot-wait-seconds',value,'--show','idle','--hold-seconds','0']),0)
                self.assertEqual([json.loads(wire)['cmd'] for wire in serial.writes],
                                 ['ping','status','emotion','emotion'])
                self.assertTrue(serial.closed)

    def test_show_longer_than_lease_renews_before_restoring_clock(self):
        clock=Clock();serial=FakeSerial(clock);client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        modules=serial_module_stubs([port('COM4',0x1a86,0x55d3)])
        with patch.dict(sys.modules,modules),patch.object(display,'open_display_port',return_value=serial), \
             patch.object(display,'DisplayClient',return_value=client), \
             patch.object(display.time,'monotonic',clock),patch.object(display.time,'sleep',clock.advance), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(display.main(['--show','thinking','--hold-seconds','12']),0)
        requests=[json.loads(value) for value in serial.writes]
        states=[r['state'] for r in requests if r['cmd']=='emotion']
        self.assertGreaterEqual(states.count('thinking'),3)
        self.assertEqual(states[-1],'clock');self.assertTrue(serial.closed)
        self.assertAlmostEqual(clock.value,12)

    def test_show_restores_clock_and_closes_only_fake_display(self):
        clock=Clock();serial=FakeSerial(clock);client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        modules=serial_module_stubs([port('COM4',0x1a86,0x55d3)])
        with patch.dict(sys.modules,modules),patch.object(display,'open_display_port',return_value=serial) as opened, \
             patch.object(display,'DisplayClient',return_value=client),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(display.main(['--show','thinking','--hold-seconds','0']),0)
        opened.assert_called_once_with('COM4')
        requests=[json.loads(value) for value in serial.writes]
        self.assertEqual([r['cmd'] for r in requests],['ping','status','emotion','emotion'])
        self.assertEqual([r.get('state') for r in requests[-2:]],['thinking','clock'])
        self.assertTrue(serial.closed)

    def test_ctrl_c_restores_clock_without_any_microphone_request(self):
        clock=Clock();serial=FakeSerial(clock);client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        modules=serial_module_stubs([port('COM4',0x1a86,0x55d3)])
        def interrupted():raise KeyboardInterrupt
        with patch.dict(sys.modules,modules),patch.object(display,'open_display_port',return_value=serial), \
             patch.object(display,'DisplayClient',return_value=client), \
             patch.object(display,'DisplayBridge',return_value=SimpleNamespace(step=interrupted)), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(display.main([]),0)
        self.assertEqual(json.loads(serial.writes[-1])['state'],'clock')
        self.assertTrue(serial.closed)

    def test_old_cli_does_not_get_emotion_commands_or_raw_echo(self):
        old=(LEGACY_MENU+' OLD_CLI_PRIVATE_DATA\n').encode()
        clock=Clock();serial=FakeSerial(clock,lambda request:old)
        client=display.DisplayClient(serial,clock=clock,ack_timeout=.2)
        modules=serial_module_stubs([port('COM4',0x1a86,0x55d3)]);error=io.StringIO()
        with patch.dict(sys.modules,modules),patch.object(display,'open_display_port',return_value=serial), \
             patch.object(display,'DisplayClient',return_value=client),contextlib.redirect_stderr(error):
            self.assertEqual(display.main(['--show','thinking','--hold-seconds','0']),1)
        self.assertEqual([json.loads(value)['cmd'] for value in serial.writes],['ping'])
        self.assertNotIn('OLD_CLI_PRIVATE_DATA',error.getvalue())
        self.assertTrue(serial.closed)

    def test_explicit_bes_port_is_rejected_before_open(self):
        modules=serial_module_stubs([port('COM3',0x0403,0x6001)])
        with patch.dict(sys.modules,modules),patch.object(display,'open_display_port') as opened, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(display.main(['--port','COM3','--show','idle']),1)
        opened.assert_not_called()


if __name__=='__main__':unittest.main()

"""One-click orchestration tests; no serial device is opened."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from launch import active_sessions, choose_port, main as launch_main, process_running, stop_session
from start import NetworkClock, AutomaticVoice, AutoVoiceDisplay, ControlHealth, wait_for_agent_stop


class AutomaticVoiceTests(unittest.TestCase):
    def test_manual_mode_actively_stops_boot_listener_once(self):
        calls=[]
        def send(value,**kwargs):calls.append(value);return {'rc':0}
        auto=AutomaticVoice(False,send=send,stop_when_disabled=True)
        auto.start_once();auto.wait();auto.start_once()
        self.assertEqual(calls,[{'action':'command','args':['auto','stop']}])
        self.assertEqual(auto.result['auto_voice_state'],'stopped')

    def test_start_is_one_board_command_even_when_called_repeatedly(self):
        calls=[]
        def send(command,**options):
            calls.append(command);return {'rc':0,'command_return_only':True}
        auto=AutomaticVoice(True,send=send)
        for _ in range(20):auto.start_once()
        auto.thread.join(1)
        self.assertEqual(calls,[{'action':'command','args':['auto','start']}])
        self.assertEqual(auto.result['auto_voice_state'],'started')

    def test_board_business_failure_does_not_claim_voice_started(self):
        auto=AutomaticVoice(True,send=lambda *a,**k:{'rc':1})
        auto.start_once();auto.thread.join(1)
        self.assertEqual(auto.result['auto_voice_state'],'failed')
        self.assertEqual(auto.result['auto_voice_rc'],1)

    def test_transport_failure_does_not_claim_voice_started(self):
        def send(*a,**k):raise TimeoutError('Control channel timed out')
        auto=AutomaticVoice(True,send=send)
        auto.start_once();auto.thread.join(1)
        self.assertEqual(auto.result['auto_voice_state'],'failed')
        self.assertIn('timed out',auto.result['auto_voice_error'])

    def test_diagnostic_mode_sends_no_voice_command(self):
        calls=[]
        auto=AutomaticVoice(False,send=lambda *a,**k:calls.append(a))
        auto.start_once()
        self.assertEqual(calls,[])
        self.assertIsNone(auto.thread)


class AutomaticStatusTests(unittest.TestCase):
    def display_state(self,state,**values):
        voice={'enabled':True,'running':True,'state':state,'last_error':0,'accepting_speech':False}
        voice.update(values)
        return {'auto_status_confirmed':True,'auto_voice_board_status':voice}

    def test_synthesizing_is_distinct_from_actual_audio_playback(self):
        display=AutoVoiceDisplay()
        text=display.message(self.display_state('正在合成语音，暂停聆听'))
        self.assertIn('尚未开始播放',text)
        text=display.message(self.display_state('正在播放回答，暂停聆听'))
        self.assertIn('正在播放回答',text)
        self.assertNotIn('尚未开始',text)

    def test_disabled_worker_draining_is_not_reported_stopped(self):
        display=AutoVoiceDisplay()
        text=display.message(self.display_state('正在停止',enabled=False))
        self.assertIn('尚未确认',text)
        self.assertNotIn('已停止',text)
        text=display.message(self.display_state('已停止',enabled=False,running=False))
        self.assertIn('已停止',text)

    def test_ready_requires_board_confirmation_that_capture_accepts_speech(self):
        display=AutoVoiceDisplay()
        text=display.message(self.display_state('等待说话'))
        self.assertIn('尚未确认',text)
        text=display.message(self.display_state('等待说话',accepting_speech=True))
        self.assertIn('可以直接',text)

    def test_wake_mode_explains_when_a_command_can_be_given(self):
        display=AutoVoiceDisplay()
        waiting=self.display_state('等待说话',accepting_speech=True,wake_enabled=True,awake=False)
        self.assertIn('请先说“你好，夕夕”或“Hello，xixi”',display.message(waiting))
        awake=self.display_state('等待说话',accepting_speech=True,wake_enabled=True,awake=True)
        self.assertIn('已唤醒',display.message(awake))
        self.assertIsNone(display.message(awake))

    def test_boot_wake_stages_require_real_capture_before_prompt(self):
        display=AutoVoiceDisplay()
        for state in ['等待唤醒词','等待提问']:
            text=display.message(self.display_state(state,accepting_speech=False))
            self.assertIn('尚未确认',text)
        text=display.message(self.display_state('正在播报唤醒应答',output_active=True))
        self.assertIn('“你好”',text)

    def test_wake_disabled_retains_direct_conversation_prompt(self):
        display=AutoVoiceDisplay()
        direct=self.display_state('等待说话',accepting_speech=True,wake_enabled=False,awake=True)
        self.assertIn('可以直接',display.message(direct))

    def test_busy_progress_is_sparse_and_does_not_suggest_more_speech(self):
        now=[0.0];display=AutoVoiceDisplay(clock=lambda:now[0])
        value=self.display_state('正在合成语音，暂停聆听',utterances=1)
        display.message(value)
        now[0]=14;self.assertIsNone(display.message(value))
        now[0]=15;text=display.message(value)
        self.assertIn('15 秒',text);self.assertIn('暂停收音',text)
        now[0]=30;self.assertIsNone(display.message(value))
        now[0]=45;self.assertIn('45 秒',display.message(value))
        value['auto_voice_board_status']['utterances']=2
        self.assertIn('尚未开始播放',display.message(value))
        now[0]=46;self.assertIsNone(display.message(value))

    def test_timeout_stop_does_not_claim_automatic_recovery(self):
        text=AutoVoiceDisplay().message(self.display_state('回答超时，自动监听已停止',enabled=False,running=False))
        self.assertIn('主动重新启动',text)
        self.assertNotIn('恢复',text)

    def test_interrupted_and_overlong_sentences_request_a_complete_retry(self):
        display=AutoVoiceDisplay()
        text=display.message(self.display_state('收音被打断，等待停顿'))
        self.assertIn('重新说完整',text)
        text=display.message(self.display_state('语句过长，等待停顿'))
        self.assertIn('分成较短',text)

    def test_voice_poll_is_read_only_and_ping_keeps_thirty_second_period(self):
        now=[0.0];calls=[]
        def probe(value,**options):
            calls.append(value['action'])
            if value['action']=='ping':return {'rc':0}
            return {'rc':0,'auto_voice':{'enabled':True,'running':True,'state':'等待说话','last_error':0}}
        health=ControlHealth(probe=probe,clock=lambda:now[0]);health.enable_voice_status()
        for at in [0,4,8,30]:
            now[0]=at;health.tick();health.thread.join(1)
        self.assertEqual(calls.count('ping'),2)
        self.assertEqual(calls.count('auto-status'),4)
        self.assertEqual(set(calls),{'ping','auto-status'})
        self.assertTrue(health.voice_snapshot()['auto_status_confirmed'])

    def test_voice_status_failure_is_separate_from_network_ping(self):
        def probe(value,**options):
            if value['action']=='ping':return {'rc':0}
            raise TimeoutError('status request timeout')
        health=ControlHealth(probe=probe);health.enable_voice_status()
        health.tick();health.thread.join(1)
        self.assertTrue(health.snapshot()['ready'])
        self.assertFalse(health.voice_snapshot()['auto_status_confirmed'])
        self.assertIn('timeout',health.voice_snapshot()['auto_status_check_error'])

    def test_unchanged_listening_state_does_not_repeat_frame_statistics(self):
        display=AutoVoiceDisplay()
        state={'auto_status_confirmed':True,'auto_voice_board_status':{
            'enabled':True,'running':True,'state':'等待说话','last_error':0,'frames':100}}
        self.assertIn('等待你说话',display.message(state))
        state['auto_voice_board_status']['frames']=1000
        self.assertIsNone(display.message(state))
        state['auto_voice_board_status']['state']='正在识别'
        self.assertIn('正在识别',display.message(state))

    def test_status_read_error_and_asr_recovery_have_different_feedback(self):
        display=AutoVoiceDisplay()
        text=display.message({'auto_status_check_error':'network timeout'})
        self.assertIn('状态查询失败',text)
        self.assertNotIn('本次语音处理失败',text)
        text=display.message({'auto_status_confirmed':True,'auto_voice_board_status':{
            'enabled':True,'running':True,'state':'语音异常，正在恢复','last_error':-5}})
        self.assertIn('本次语音处理失败',text)
        self.assertIn('两短音',text)


class LauncherTests(unittest.TestCase):
    def test_current_process_is_detected_without_sending_signals(self):
        self.assertTrue(process_running(os.getpid()))

    def test_dead_history_does_not_hide_the_current_active_session(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for name,pid in [('old',101),('current',202)]:
                folder=root/name;folder.mkdir()
                (folder/'status.json').write_text(json.dumps({'phase':'connected','forwarder_pid':pid}))
            with patch('launch.process_running',side_effect=lambda pid:pid==202):
                self.assertEqual(active_sessions(root),[root/'current/status.json'])

    def test_unreadable_state_is_not_mistaken_for_no_session(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);folder=root/'test';folder.mkdir()
            (folder/'status.json').write_text('{')
            with self.assertRaisesRegex(RuntimeError,'暂时无法读取'):
                active_sessions(root)

    def test_duplicate_entry_does_not_launch_another_forwarder(self):
        with patch('sys.argv',['launch.py','--auto-voice']),patch('launch.preflight',return_value='COM3'), \
             patch('launch.active_sessions',return_value=[Path('existing/status.json')]), \
             patch('launch.subprocess.call') as run,contextlib.redirect_stdout(io.StringIO()), \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(launch_main(),1)
        run.assert_not_called()

    def test_only_matching_ftdi_is_selected(self):
        ports=[SimpleNamespace(device='COM1',vid=1,pid=2),
               SimpleNamespace(device='COM7',vid=0x0403,pid=0x6001)]
        self.assertEqual(choose_port(ports),'COM7')
        self.assertEqual(choose_port(ports,'com1'),'COM1')

    def test_multiple_boards_require_explicit_selection(self):
        ports=[SimpleNamespace(device=n,vid=0x0403,pid=0x6001) for n in ['COM3','COM7']]
        with self.assertRaises(RuntimeError):choose_port(ports)

    def test_default_entry_keeps_automatic_wake_listening(self):
        with patch('sys.argv',['launch.py']),patch('launch.preflight',return_value='COM7'), \
             patch('launch.active_sessions',return_value=[]), \
             patch('launch.subprocess.call',return_value=0) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(launch_main(),0)
        command=run.call_args.args[0]
        self.assertIn('--auto-voice',command)
        self.assertEqual(command[command.index('--port')+1],'COM7')
        self.assertNotIn('listen',command)

    def test_explicit_manual_entry_requests_stop(self):
        with patch('sys.argv',['launch.py','--manual-voice']),patch('launch.preflight',return_value='COM7'), \
             patch('launch.active_sessions',return_value=[]), \
             patch('launch.subprocess.call',return_value=0) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(launch_main(),0)
        self.assertIn('--manual-voice',run.call_args.args[0])
        self.assertNotIn('--auto-voice',run.call_args.args[0])

    def test_explicit_voice_entry_enables_board_automatic_voice(self):
        with patch('sys.argv',['launch.py','--auto-voice']),patch('launch.preflight',return_value='COM7'), \
             patch('launch.active_sessions',return_value=[]), \
             patch('launch.subprocess.call',return_value=0) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(launch_main(),0)
        self.assertIn('--auto-voice',run.call_args.args[0])

    def test_check_only_does_not_start_forwarder_or_record(self):
        with patch('sys.argv',['launch.py','--check-only']),patch('launch.preflight',return_value='COM3'), \
             patch('launch.subprocess.call') as run,contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(launch_main(),0)
        run.assert_not_called()

    def test_stop_targets_local_session_and_waits_for_confirmation(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);session=root/'test';session.mkdir()
            status=session/'status.json';status.write_text('{"phase":"connected"}')
            def forwarder():
                until=time.monotonic()+2
                while time.monotonic()<until:
                    if (session/'stop.request').exists():
                        status.write_text('{"phase":"stopped"}');return
                    time.sleep(.01)
            worker=threading.Thread(target=forwarder);worker.start()
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(stop_session(root,wait_seconds=3),0)
                self.assertTrue((session/'stop.request').exists())
            finally:worker.join(3)


class ShutdownProgressTests(unittest.TestCase):
    def test_slow_shutdown_reports_progress_and_waits_for_real_completion(self):
        now=[0];messages=[]
        class Port:
            def read(self,_):
                now[0]+=5
                return b'[agent] Shutdown complete.' if now[0]>=35 else b''
        reply=wait_for_agent_stop(Port(),b'stop requested',clock=lambda:now[0],
                                  emit=lambda text,**options:messages.append(text))
        self.assertIn(b'Shutdown complete.',reply)
        self.assertEqual(len(messages),3)
        self.assertEqual(now[0],35)

    def test_elapsed_timeout_does_not_invent_shutdown_confirmation(self):
        now=[0]
        class Port:
            def read(self,_):now[0]+=5;return b''
        reply=wait_for_agent_stop(Port(),b'',timeout=20,clock=lambda:now[0],emit=lambda *a,**k:None)
        self.assertNotIn(b'Shutdown complete.',reply)


class NetworkClockTests(unittest.TestCase):
    def test_sync_once_per_confirmed_link_and_on_reconnect(self):
        calls=[]
        def send(value,**kwargs):calls.append(value);return {'rc':0,'epoch':value['epoch']}
        clock=NetworkClock(send=send,epoch=lambda:1793574000)
        clock.update(False);self.assertEqual(calls,[])
        clock.update(True);clock.wait();clock.update(True)
        self.assertEqual(len(calls),1)
        clock.update(False);clock.update(True);clock.wait()
        self.assertEqual(len(calls),2)
        self.assertEqual(clock.result['news_clock_sync_state'],'confirmed')

    def test_failure_does_not_claim_synced_or_spin(self):
        now=[0];calls=[]
        def send(value,**kwargs):calls.append(value);return {'rc':-1}
        clock=NetworkClock(send=send,clock=lambda:now[0],epoch=lambda:1793574000)
        clock.update(True);clock.wait();clock.update(True)
        self.assertEqual(len(calls),1);self.assertEqual(clock.result['news_clock_sync_state'],'failed')
        now[0]=61;clock.update(True);clock.wait();self.assertEqual(len(calls),2)


if __name__=='__main__':unittest.main()

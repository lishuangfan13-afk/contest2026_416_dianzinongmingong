"""Pure mock tests: never register tasks, open serial ports, or supply networks."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parents[2] / 'tools/pc_link'
sys.path.insert(0, str(HERE))
import boot_startup as startup

SID = 'S-1-5-21-111-222-333-1001'
TAGS = {'t': startup.NS}


def result(code=0, stdout=b''):
    return SimpleNamespace(returncode=code, stdout=stdout, stderr=b'private diagnostic')


class StartupTests(unittest.TestCase):
    def setUp(self):
        # Even a test missing its narrower run_command patch cannot contact Windows.
        self.process = patch.object(startup.subprocess, 'run',
                                    side_effect=AssertionError('Real subprocess forbidden'))
        self.process.start()
        self.addCleanup(self.process.stop)
        self.python = patch.object(startup, 'python_command',
                                   return_value='C:\\Private User\\Python\\pythonw.exe')
        self.python.start()
        self.addCleanup(self.python.stop)
        self.config = startup.make_config(SID, 'com20', Path.cwd() / '私密 用户' / '状态 & 数据')
        self.xml = startup.task_xml(self.config)
        self.snapshot = patch.object(startup, 'read_watcher', return_value={})
        self.snapshot.start()
        self.addCleanup(self.snapshot.stop)

    def test_xml_round_trip_chinese_spaces_and_ampersands(self):
        tree = ET.fromstring(self.xml)
        arguments = tree.findtext('t:Actions/t:Exec/t:Arguments', namespaces=TAGS)
        self.assertEqual(arguments, subprocess.list2cmdline([
            self.config['watcher'], '--port', 'COM20', '--state-dir',
            self.config['state_dir'], '--duration', '0']))
        self.assertIn('"' + self.config['state_dir'] + '"', arguments)
        self.assertEqual(tree.findtext('t:Actions/t:Exec/t:WorkingDirectory', namespaces=TAGS), str(startup.ROOT))
        self.assertEqual(startup.owned_config(self.xml, SID), self.config)
        self.assertIn('&amp;', self.xml.decode('utf-16'))

    def test_scheduler_normalizes_encoding_default_fields_and_trigger_account(self):
        tree = ET.fromstring(self.xml)
        for parent, child in [('t:Triggers/t:LogonTrigger', 'Enabled'),
                              ('t:Principals/t:Principal', 'RunLevel'),
                              ('t:Settings', 'Enabled')]:
            element = tree.find(parent, TAGS)
            element.remove(element.find('t:' + child, TAGS))
        tree.find('t:Triggers/t:LogonTrigger/t:UserId', TAGS).text = 'MACHINE\\user'
        # Actual schtasks output can have UTF-8 bytes and a UTF-16 declaration.
        xml = ET.tostring(tree, encoding='unicode')
        xml = ('<?xml version="1.0" encoding="UTF-16"?>' + xml).encode('utf-8')
        with patch.object(startup, 'run_command', return_value=result(stdout=b'MACHINE\\user\r\n')):
            self.assertEqual(startup.owned_config(xml, SID), self.config)
        with patch.object(startup, 'run_command', return_value=result(stdout=b'MACHINE\\other\r\n')):
            with self.assertRaises(startup.StartupError):
                startup.owned_config(xml, SID)

    def test_watch_script_and_working_directory_with_unicode_spaces(self):
        config = dict(self.config, watcher='C:\\中文 项目\\tools\\pc_link\\boot_watch.py',
                      cwd='C:\\中文 项目')
        tree = ET.fromstring(startup.task_xml(config))
        arguments = tree.findtext('t:Actions/t:Exec/t:Arguments', namespaces=TAGS)
        self.assertTrue(arguments.startswith('"' + config['watcher'] + '"'))
        self.assertEqual(tree.findtext('t:Actions/t:Exec/t:WorkingDirectory', namespaces=TAGS), config['cwd'])
        self.assertEqual(tree.findtext('t:Actions/t:Exec/t:Command', namespaces=TAGS), config['python'])

    def test_xml_is_current_user_least_privilege_and_persistent(self):
        tree = ET.fromstring(self.xml)
        checks = {'t:Triggers/t:LogonTrigger/t:UserId': SID,
                  't:Principals/t:Principal/t:UserId': SID,
                  't:Principals/t:Principal/t:LogonType': 'InteractiveToken',
                  't:Principals/t:Principal/t:RunLevel': 'LeastPrivilege',
                  't:Settings/t:MultipleInstancesPolicy': 'IgnoreNew',
                  't:Settings/t:ExecutionTimeLimit': 'PT0S',
                  't:Settings/t:AllowHardTerminate': 'false',
                  't:Settings/t:RestartOnFailure/t:Count': '3'}
        for path, expected in checks.items():
            self.assertEqual(tree.findtext(path, namespaces=TAGS), expected)
        self.assertNotIn('Password', self.xml.decode('utf-16'))
        self.assertNotEqual(startup.task_name(SID), startup.task_name(SID + '2'))

    def test_python_without_pythonw_uses_create_no_window(self):
        config = dict(self.config, python='C:\\中文 Python\\python.exe')
        arguments = startup.watcher_arguments(config)
        self.assertTrue(arguments.startswith('-c '))
        self.assertIn('creationflags=0x08000000', arguments)
        self.assertIn('FreeConsole()', arguments)
        self.assertNotIn('shell=True', arguments)
        self.assertIn('boot_watch.py', arguments)

    def test_pythonw_is_preferred_when_adjacent(self):
        self.python.stop()
        with patch.object(startup.sys, 'executable', str(Path.cwd() / '中文 Python' / 'python.exe')), \
             patch.object(startup.Path, 'is_file', return_value=True):
            self.assertEqual(Path(startup.python_command()).name, 'pythonw.exe')
        with patch.object(startup.Path, 'is_file', return_value=False):
            self.assertEqual(startup.python_command(), str(Path(sys.executable).resolve()))

    def test_reject_foreign_owner_changed_exec_and_malformed_xml(self):
        for bad in (b'broken XML', startup.task_xml(dict(self.config, owner='foreign')),
                    startup.task_xml(dict(self.config, sid=SID + '2')),
                    startup.task_xml(dict(self.config, watcher='other.py'))):
            with self.assertRaises(startup.StartupError):
                startup.owned_config(bad, SID)
        tree = ET.fromstring(self.xml)
        tree.find('t:Actions/t:Exec/t:Arguments', TAGS).text = 'unrelated.py'
        with self.assertRaises(startup.StartupError):
            startup.owned_config(ET.tostring(tree), SID)

    def test_modified_logon_or_persistence_settings_are_refused(self):
        for path, value in (('t:Triggers/t:LogonTrigger/t:UserId', SID + '2'),
                            ('t:Settings/t:Enabled', 'false'),
                            ('t:Settings/t:MultipleInstancesPolicy', 'Parallel'),
                            ('t:Settings/t:ExecutionTimeLimit', 'PT1H'),
                            ('t:Settings/t:AllowHardTerminate', 'true')):
            tree = ET.fromstring(self.xml)
            tree.find(path, TAGS).text = value
            with self.assertRaises(startup.StartupError):
                startup.owned_config(ET.tostring(tree), SID)

    def test_absent_uninstall_does_not_stop_or_delete(self):
        with patch.object(startup, 'query_task', return_value=None), \
             patch.object(startup, 'run_command') as run, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(startup.uninstall(SID), 0)
            self.assertIn('no task was deleted', output.getvalue())
            run.assert_not_called()

    def test_immediate_start_failure_keeps_install_and_has_no_success_message(self):
        with patch.object(startup, 'query_task', side_effect=[None, self.xml]), \
             patch.object(startup, 'run_command', side_effect=[result(), result(1)]) as run, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaisesRegex(startup.StartupError, 'remains installed'):
                startup.install(SID, 'COM20', Path(self.config['state_dir']), start=True)
            self.assertEqual(output.getvalue(), '')
            self.assertTrue(all('/Delete' not in call.args[0] for call in run.call_args_list))

    def test_query_confirmed_absence_and_failed_listing(self):
        with patch.object(startup, 'run_command', return_value=result(stdout=b'"\\Other","N/A","Ready"\r\n')) as run:
            self.assertIsNone(startup.query_task(startup.task_name(SID)))
            self.assertEqual(run.call_count, 1)
        with patch.object(startup, 'run_command', return_value=result(1)):
            with self.assertRaises(startup.StartupError):
                startup.query_task(startup.task_name(SID))

    def test_query_existing_task_fetches_xml_and_rejects_failed_read(self):
        listing = ('"\\' + startup.task_name(SID) + '","N/A","Running"\r\n').encode()
        with patch.object(startup, 'run_command', side_effect=[result(stdout=listing), result(stdout=self.xml)]) as run:
            self.assertEqual(startup.query_task(startup.task_name(SID)), self.xml)
            self.assertEqual(run.call_args.args[0][-1], '/XML')
        with patch.object(startup, 'run_command', side_effect=[result(stdout=listing), result(1)]):
            with self.assertRaises(startup.StartupError):
                startup.query_task(startup.task_name(SID))

    def test_install_readback_and_optional_start_use_same_task(self):
        captured = []
        def execute(command, timeout=30):
            captured.append(command)
            if '/Create' in command:
                xml_path = Path(command[command.index('/XML') + 1])
                self.assertEqual(startup.owned_config(xml_path.read_bytes(), SID), self.config)
            return result()
        with patch.object(startup, 'query_task', side_effect=[None, self.xml]), \
             patch.object(startup, 'run_command', side_effect=execute), \
             patch.object(startup, 'wait_for_watcher', return_value={}) as ready, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(startup.install(SID, 'COM20', Path(self.config['state_dir']), start=True), 0)
        self.assertEqual([command[1] for command in captured], ['/Create', '/Run'])
        self.assertEqual(captured[0][captured[0].index('/TN') + 1], captured[1][-1])
        self.assertNotIn('/F', captured[0])
        self.assertNotIn('/RU', captured[0])
        self.assertNotIn('/RP', captured[0])
        self.assertNotIn(self.config['state_dir'], output.getvalue())
        self.assertNotIn(SID, output.getvalue())
        self.assertEqual(ready.call_args.args[0], self.config)
        self.assertIn('runtime confirmed', output.getvalue())

    def test_identical_install_is_idempotent(self):
        with patch.object(startup, 'query_task', return_value=self.xml), \
             patch.object(startup, 'run_command') as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(startup.install(SID, 'COM20', Path(self.config['state_dir'])), 0)
        run.assert_not_called()

    def test_install_failure_or_missing_readback_does_not_claim_success(self):
        for code, queries in ((1, [None]), (0, [None, None])):
            with patch.object(startup, 'query_task', side_effect=queries), \
                 patch.object(startup, 'run_command', return_value=result(code)), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                with self.assertRaises(startup.StartupError):
                    startup.install(SID, 'COM20', Path(self.config['state_dir']))
                self.assertEqual(output.getvalue(), '')

    def test_reconfigure_stops_original_state_before_replace(self):
        new_state = Path(self.config['state_dir']) / 'new'
        new_config = startup.make_config(SID, 'COM21', new_state)
        events = []
        with patch.object(startup, 'query_task', side_effect=[self.xml, startup.task_xml(new_config)]), \
             patch.object(startup, 'stop_watcher', side_effect=lambda path: events.append(('stop', path))), \
             patch.object(startup, 'run_command', side_effect=lambda command: events.append(('create', command)) or result()), \
             contextlib.redirect_stdout(io.StringIO()):
            startup.install(SID, 'COM21', new_state)
        self.assertEqual(events[0], ('stop', Path(self.config['state_dir'])))
        self.assertIn('/F', events[1][1])

    def test_failed_stop_prevents_replace_or_delete(self):
        with patch.object(startup, 'query_task', return_value=self.xml), \
             patch.object(startup, 'run_command', return_value=result(1)) as run:
            for action in (lambda: startup.uninstall(SID),
                           lambda: startup.install(SID, 'COM21', Path(self.config['state_dir']))):
                with self.assertRaisesRegex(startup.StartupError, 'preserved'):
                    action()
            self.assertEqual(run.call_count, 2)
            for call in run.call_args_list:
                self.assertIn('--stop', call.args[0])
                self.assertEqual(call.kwargs['timeout'], 280)
                self.assertNotIn('/Delete', call.args[0])
                self.assertNotIn('/Create', call.args[0])

    def test_uninstall_stops_rechecks_deletes_and_confirms_absence(self):
        events = []
        with patch.object(startup, 'query_task', side_effect=[self.xml, self.xml, None]), \
             patch.object(startup, 'stop_watcher', side_effect=lambda path: events.append('stop')), \
             patch.object(startup, 'run_command', side_effect=lambda command: events.append(command[1]) or result()), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(startup.uninstall(SID), 0)
        self.assertEqual(events, ['stop', '/Delete'])
        self.assertIn('absence verified', output.getvalue())

    def test_changed_task_during_stop_is_not_deleted(self):
        changed = startup.task_xml(dict(self.config, port='COM21'))
        with patch.object(startup, 'query_task', side_effect=[self.xml, changed]), \
             patch.object(startup, 'stop_watcher'), patch.object(startup, 'run_command') as run:
            with self.assertRaisesRegex(startup.StartupError, 'changed'):
                startup.uninstall(SID)
            run.assert_not_called()

    def test_delete_failure_or_remaining_task_never_claims_success(self):
        for code in (0, 1):
            with patch.object(startup, 'query_task', return_value=self.xml), \
                 patch.object(startup, 'stop_watcher'), \
                 patch.object(startup, 'run_command', return_value=result(code)), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                with self.assertRaisesRegex(startup.StartupError, 'unconfirmed'):
                    startup.uninstall(SID)
                self.assertEqual(output.getvalue(), '')

    def test_status_absent_installed_and_no_private_values(self):
        for xml, expected in ((None, 'not installed'), (self.xml, 'ownership verified')):
            with patch.object(startup, 'query_task', return_value=xml), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(startup.status(SID), 0)
                self.assertIn(expected, output.getvalue())
                for private in (SID, self.config['port'], self.config['state_dir'], self.config['python']):
                    self.assertNotIn(private, output.getvalue())

    def test_stop_uses_installed_state_and_rejects_different_override(self):
        with patch.object(startup, 'query_task', return_value=self.xml), \
             patch.object(startup, 'stop_watcher') as stop, \
             contextlib.redirect_stdout(io.StringIO()):
            startup.stop(SID)
            stop.assert_called_once_with(Path(self.config['state_dir']))
            with self.assertRaisesRegex(startup.StartupError, 'differs'):
                startup.stop(SID, Path.cwd() / 'other')
            self.assertEqual(stop.call_count, 1)

    def test_stop_command_targets_only_watcher_without_kill_or_raw_output(self):
        with patch.object(startup, 'run_command', return_value=result()) as run:
            startup.stop_watcher(Path(self.config['state_dir']))
        self.assertEqual(run.call_args.args[0], [sys.executable, str(startup.WATCHER),
                         '--stop', '--state-dir', self.config['state_dir']])

    def test_current_user_sid_is_validated_without_echoing_name(self):
        with patch.object(startup, 'run_command', return_value=result(stdout=(
                '"PRIVATE\\username","' + SID + '"\r\n').encode())):
            self.assertEqual(startup.current_user_sid(), SID)
        with patch.object(startup, 'run_command', return_value=result(stdout=b'bad')):
            with self.assertRaises(startup.StartupError):
                startup.current_user_sid()

    def test_command_has_no_console_shell_or_inherited_output(self):
        with patch.object(startup.subprocess, 'run', return_value=result()) as run:
            startup.run_command(['synthetic.exe'], timeout=280)
        self.assertEqual(run.call_args.kwargs['creationflags'], startup.NO_WINDOW)
        self.assertEqual(run.call_args.kwargs['stdin'], subprocess.DEVNULL)
        self.assertEqual(run.call_args.kwargs['stdout'], subprocess.PIPE)
        self.assertEqual(run.call_args.kwargs['stderr'], subprocess.PIPE)
        self.assertNotIn('shell', run.call_args.kwargs)

    def test_command_errors_are_redacted(self):
        for error in (OSError('PRIVATE PATH'), subprocess.TimeoutExpired('PRIVATE COMMAND', 30)):
            with patch.object(startup.subprocess, 'run', side_effect=error):
                with self.assertRaises(startup.StartupError) as caught:
                    startup.run_command(['synthetic.exe'])
                self.assertNotIn('PRIVATE', str(caught.exception))

    def test_cli_failure_is_redacted_and_returns_nonzero(self):
        with patch.object(startup.os, 'name', 'nt'), \
             patch.object(startup, 'current_user_sid', return_value=SID), \
             patch.object(startup, 'status', side_effect=OSError('PRIVATE PATH')), \
             contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(startup.main(['--status']), 1)
            self.assertNotIn('PRIVATE', output.getvalue())

    def test_port_validation(self):
        for port in (None, '', 'COM0', 'COM20 & unrelated', 'COM-1'):
            with self.assertRaises(startup.StartupError):
                startup.make_config(SID, port, Path.cwd())

    def live_snapshot(self, **changes):
        value = {'schema': 1, 'pid': 1234, 'port': 'COM20',
                 'phase': 'waiting-choice', 'serial_open': True,
                 'serial_close_confirmed': False, 'cleanup_confirmed': False,
                 'instance_id': 'a' * 32, 'started_at': 95.0, 'updated_at': 100.0}
        value.update(changes)
        return value

    def test_start_passes_verified_config_and_requires_actual_runtime(self):
        with patch.object(startup, 'run_command', return_value=result()) as run, \
             patch.object(startup.time, 'time', return_value=100.0), \
             patch.object(startup, 'wait_for_watcher', return_value=self.live_snapshot()) as ready:
            value = startup.start_task(startup.task_name(SID), self.config)
        self.assertEqual(value['phase'], 'waiting-choice')
        run.assert_called_once_with(['schtasks.exe', '/Run', '/TN', startup.task_name(SID)])
        ready.assert_called_once_with(self.config, 100.0)

    def test_idempotent_start_uses_existing_verified_state_directory(self):
        with patch.object(startup, 'query_task', return_value=self.xml), \
             patch.object(startup, 'start_task') as start, \
             contextlib.redirect_stdout(io.StringIO()) as output:
            startup.install(SID, 'COM20', Path(self.config['state_dir']), start=True)
        start.assert_called_once_with(startup.task_name(SID), self.config)
        self.assertIn('runtime confirmed', output.getvalue())

    def test_runtime_rejects_stale_pid_wrong_port_invalid_instance_and_times(self):
        changes = [{'schema': True}, {'schema': 2}, {'port': 'COM21'}, {'pid': True}, {'pid': -1},
                   {'instance_id': ''}, {'instance_id': 'not-a-nonce'},
                   {'updated_at': 80.0, 'started_at': 79.0},
                   {'updated_at': 102.0}, {'updated_at': float('nan')},
                   {'started_at': float('inf')}, {'started_at': True},
                   {'started_at': 101.0}]
        with patch.object(startup, 'process_running', return_value=True):
            self.assertTrue(startup.watcher_ready(self.live_snapshot(), self.config, 100.0))
            for change in changes:
                with self.subTest(change=change):
                    self.assertFalse(startup.watcher_ready(self.live_snapshot(**change), self.config, 100.0))
        for alive in (False, None):
            with patch.object(startup, 'process_running', return_value=alive):
                self.assertFalse(startup.watcher_ready(self.live_snapshot(), self.config, 100.0))

    def test_valid_phases_require_actual_port_or_live_child(self):
        with patch.object(startup, 'process_running', return_value=True):
            for phase in ('failed', 'stopped', 'port-busy', 'stopping-ppp', 'unknown'):
                self.assertFalse(startup.watcher_ready(self.live_snapshot(phase=phase), self.config, 100.0))
            self.assertFalse(startup.watcher_ready(self.live_snapshot(serial_open=False), self.config, 100.0))
            self.assertTrue(startup.watcher_ready(self.live_snapshot(phase='polling-wireless'), self.config, 100.0))
            released = self.live_snapshot(phase='wireless-selected', serial_open=False, serial_close_confirmed=True)
            self.assertTrue(startup.watcher_ready(released, self.config, 100.0))
            attaching = dict(released, phase='attaching-wired', attach_pid=4321)
            self.assertTrue(startup.watcher_ready(attaching, self.config, 100.0))
        with patch.object(startup, 'process_running', side_effect=lambda pid: pid == 1234):
            self.assertFalse(startup.watcher_ready(attaching, self.config, 100.0))

    def test_wait_rejects_old_reused_pid_snapshot_until_fresh_heartbeat(self):
        reads = [self.live_snapshot(updated_at=99.5), self.live_snapshot(updated_at=99.5),
                 self.live_snapshot(updated_at=100.2)]
        ticks = [0.0]
        def pause(seconds): ticks[0] += seconds
        with patch.object(startup, 'read_watcher', side_effect=reads), \
             patch.object(startup, 'process_running', return_value=True):
            value = startup.wait_for_watcher(self.config, 100.0, timeout=1,
                clock=lambda: ticks[0], wall_clock=lambda: 100.0 + ticks[0], pause=pause)
        self.assertEqual(value['updated_at'], 100.2)
        self.assertAlmostEqual(ticks[0], .4)

    def test_scheduler_ack_with_failed_stopped_missing_or_old_state_times_out(self):
        for value in ({}, self.live_snapshot(phase='failed'), self.live_snapshot(phase='stopped'),
                      self.live_snapshot(updated_at=99.9)):
            ticks = [0.0]
            def pause(seconds): ticks[0] += seconds
            with self.subTest(value=value), \
                 patch.object(startup, 'read_watcher', return_value=value), \
                 patch.object(startup, 'process_running', return_value=True):
                with self.assertRaisesRegex(startup.StartupError, 'readiness was not confirmed'):
                    startup.wait_for_watcher(self.config, 100.0, timeout=.5,
                        clock=lambda: ticks[0], wall_clock=lambda: 100.0 + ticks[0], pause=pause)
                self.assertAlmostEqual(ticks[0], .5)

    def test_startup_runtime_failure_keeps_task_and_does_not_claim_success(self):
        with patch.object(startup, 'query_task', return_value=self.xml), \
             patch.object(startup, 'run_command', return_value=result()) as run, \
             patch.object(startup, 'wait_for_watcher', side_effect=startup.StartupError('runtime unconfirmed')), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(startup.StartupError):
                startup.install(SID, 'COM20', Path(self.config['state_dir']), start=True)
        self.assertEqual(output.getvalue(), '')
        self.assertEqual(run.call_count, 1)
        self.assertIn('/Run', run.call_args.args[0])

    def test_status_reports_ready_stopped_failed_and_unconfirmed_without_paths(self):
        cases = [(self.live_snapshot(), 'ready and waiting'),
                 (self.live_snapshot(phase='failed'), 'explicit recovery'),
                 (self.live_snapshot(phase='stopped', cleanup_confirmed=True), 'not waiting'),
                 ({}, 'installation alone'),
                 (self.live_snapshot(phase='port-busy'), 'serial standby readiness is unconfirmed')]
        for value, message in cases:
            with self.subTest(message=message), \
                 patch.object(startup, 'query_task', return_value=self.xml), \
                 patch.object(startup, 'read_watcher', return_value=value), \
                 patch.object(startup.time, 'time', return_value=100.0), \
                 patch.object(startup, 'process_running', return_value=True), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                startup.status(SID)
            self.assertIn(message, output.getvalue())
            self.assertIn('does not confirm the board Internet', output.getvalue())
            for private in (SID, self.config['port'], self.config['state_dir'], self.config['python']):
                self.assertNotIn(private, output.getvalue())

    def test_read_snapshot_uses_exact_installed_path_and_redacts_file_errors(self):
        self.snapshot.stop()
        with patch.object(startup, 'read_status', return_value=self.live_snapshot()) as read:
            self.assertEqual(startup.read_watcher(self.config)['pid'], 1234)
        read.assert_called_once_with(Path(self.config['state_dir']) / 'watch.json')
        for error in (FileNotFoundError('PRIVATE PATH'), ValueError('PRIVATE DATA')):
            with patch.object(startup, 'read_status', side_effect=error):
                self.assertEqual(startup.read_watcher(self.config), {})


if __name__ == '__main__':
    unittest.main()

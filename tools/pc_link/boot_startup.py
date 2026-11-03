#!/usr/bin/env python3
"""Manage this Windows user's reversible boot watcher logon task.

Installation is explicit; importing this module never contacts Task Scheduler.
No password, administrator token, process termination or network setup is used.
Task command output and stored paths are deliberately not echoed to the console.
"""
import argparse
import csv
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

from launch import process_running, read_status

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WATCHER = HERE / 'boot_watch.py'
STATE = ROOT / 'out/pc_link_boot'
NS = 'http://schemas.microsoft.com/windows/2004/02/mit/task'
OWNER = 'Zhaoxi.PcLink.BootWatcher.v1'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0x08000000)
WATCHER_FRESH_SECONDS = 15
WATCHER_START_SECONDS = 20
ET.register_namespace('', NS)


class StartupError(RuntimeError):
    """An intentionally non-sensitive message suitable for console output."""


def run_command(command, timeout=30):
    """No shell, inherited stdin, task output, or console window."""
    try:
        return subprocess.run(command, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              creationflags=NO_WINDOW, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise StartupError('Windows command failed or timed out; operation is unconfirmed.') from None


def decode_output(raw):
    if isinstance(raw, str):
        return raw
    if raw.startswith((b'\xff\xfe', b'\xfe\xff')):
        return raw.decode('utf-16')
    # XML from redirected schtasks may be UTF-16 without a BOM.
    if b'\x00' in raw[:100]:
        return raw.decode('utf-16-le')
    try:
        return raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        return raw.decode('mbcs')


def current_user_sid():
    result = run_command(['whoami.exe', '/user', '/fo', 'csv', '/nh'])
    if result.returncode:
        raise StartupError('Unable to identify the current Windows user.')
    rows = list(csv.reader(io.StringIO(decode_output(result.stdout))))
    if len(rows) != 1 or len(rows[0]) != 2 or not re.fullmatch(r'S-1-5-(?:\d+-)*\d+', rows[0][1]):
        raise StartupError('Unable to identify the current Windows user.')
    return rows[0][1]


def task_name(sid):
    # Fixed for each user, avoiding collisions on shared Windows computers.
    return 'ZhaoxiPcLinkBootWatch-' + sid


def python_command():
    executable = Path(sys.executable).resolve()
    windowless = executable.with_name('pythonw.exe')
    if executable.name.lower() == 'pythonw.exe':
        return str(executable)
    return str(windowless if windowless.is_file() else executable)


def watcher_arguments(config):
    command = [config['watcher'], '--port', config['port'],
               '--state-dir', config['state_dir'], '--duration', '0']
    if Path(config['python']).name.lower() == 'pythonw.exe':
        return subprocess.list2cmdline(command)
    # Some embedded Python distributions omit pythonw. Detach this short helper
    # from its console and launch the actual persistent watcher with NO_WINDOW.
    child = [config['python']] + command
    code = ('import ctypes,subprocess,sys;'
            'ctypes.windll.kernel32.FreeConsole();'
            'sys.exit(subprocess.call(' + repr(child) + ',creationflags=0x08000000))')
    return subprocess.list2cmdline(['-c', code])


def make_config(sid, port, state_dir):
    if not port or not re.fullmatch(r'COM[1-9][0-9]*', port.upper()):
        raise StartupError('Installation requires a Windows COM port (for example COM20).')
    return {'owner': OWNER, 'sid': sid, 'port': port.upper(),
            'state_dir': str(state_dir.resolve()), 'python': python_command(),
            'watcher': str(WATCHER), 'cwd': str(ROOT)}


def task_xml(config):
    def add(parent, tag, text=None, **attrs):
        item = ET.SubElement(parent, '{' + NS + '}' + tag, attrs)
        if text is not None:
            item.text = text
        return item
    task = ET.Element('{' + NS + '}Task', {'version': '1.2'})
    registration = add(task, 'RegistrationInfo')
    add(registration, 'Description', OWNER)
    add(registration, 'Source', json.dumps(config, ensure_ascii=False, sort_keys=True))
    triggers = add(task, 'Triggers')
    logon = add(triggers, 'LogonTrigger')
    add(logon, 'Enabled', 'true')
    add(logon, 'UserId', config['sid'])
    principals = add(task, 'Principals')
    principal = add(principals, 'Principal', id='CurrentUser')
    add(principal, 'UserId', config['sid'])
    add(principal, 'LogonType', 'InteractiveToken')
    add(principal, 'RunLevel', 'LeastPrivilege')
    settings = add(task, 'Settings')
    add(settings, 'MultipleInstancesPolicy', 'IgnoreNew')
    add(settings, 'DisallowStartIfOnBatteries', 'false')
    add(settings, 'StopIfGoingOnBatteries', 'false')
    add(settings, 'AllowHardTerminate', 'false')
    add(settings, 'StartWhenAvailable', 'true')
    add(settings, 'RunOnlyIfNetworkAvailable', 'false')
    add(settings, 'Enabled', 'true')
    add(settings, 'Hidden', 'true')
    add(settings, 'ExecutionTimeLimit', 'PT0S')
    restart = add(settings, 'RestartOnFailure')
    add(restart, 'Interval', 'PT1M')
    add(restart, 'Count', '3')
    actions = add(task, 'Actions', Context='CurrentUser')
    execute = add(actions, 'Exec')
    add(execute, 'Command', config['python'])
    add(execute, 'Arguments', watcher_arguments(config))
    add(execute, 'WorkingDirectory', config['cwd'])
    return ET.tostring(task, encoding='utf-16', xml_declaration=True)


def query_task(name):
    # A failed /Query /TN is not proof of absence (permissions/service failures).
    # Confirm absence using a successful listing, independent of localized text.
    result = run_command(['schtasks.exe', '/Query', '/FO', 'CSV', '/NH'])
    if result.returncode:
        raise StartupError('Unable to query Task Scheduler; task state is unknown.')
    rows = csv.reader(io.StringIO(decode_output(result.stdout)))
    if not any(row and row[0].lstrip('\\').casefold() == name.casefold() for row in rows):
        return None
    result = run_command(['schtasks.exe', '/Query', '/TN', name, '/XML'])
    if result.returncode:
        raise StartupError('Unable to read the installed task; task state is unknown.')
    return result.stdout


def owned_config(xml, sid):
    """Refuse to replace/delete a task with mismatched owner or executable."""
    try:
        # schtasks may emit console-encoded bytes with a UTF-16 declaration.
        task = ET.fromstring(decode_output(xml))
        def text(path, default=None):
            return task.findtext(path, default=default, namespaces={'t': NS})
        def same_user(value):
            if value == sid:
                return True
            if not value or value.startswith('S-') or '\\' not in value:
                return False
            # Scheduler canonicalizes a trigger SID to DOMAIN\\user, while
            # retaining the principal SID. Match only this token's account.
            result = run_command(['whoami.exe'])
            return result.returncode == 0 and value.casefold() == decode_output(result.stdout).strip().casefold()
        config = json.loads(text('t:RegistrationInfo/t:Source'))
        expected_keys = {'owner', 'sid', 'port', 'state_dir', 'python', 'watcher', 'cwd'}
        valid = (isinstance(config, dict) and set(config) == expected_keys
                 and all(isinstance(value, str) for value in config.values())
                 and config['owner'] == OWNER and config['sid'] == sid
                 and config['watcher'] == str(WATCHER) and config['cwd'] == str(ROOT)
                 and config['python'] == python_command()
                 and Path(config['state_dir']).is_absolute()
                 and re.fullmatch(r'COM[1-9][0-9]*', config['port'])
                 and text('t:RegistrationInfo/t:Description') == OWNER
                 and text('t:Principals/t:Principal/t:UserId') == sid
                 and text('t:Principals/t:Principal/t:LogonType') == 'InteractiveToken'
                 and text('t:Principals/t:Principal/t:RunLevel', 'LeastPrivilege') == 'LeastPrivilege'
                 and len(task.findall('t:Principals/t:Principal', {'t': NS})) == 1
                 and len(task.findall('t:Triggers/*', {'t': NS})) == 1
                 and same_user(text('t:Triggers/t:LogonTrigger/t:UserId'))
                 and text('t:Triggers/t:LogonTrigger/t:Enabled', 'true') == 'true'
                 and text('t:Settings/t:Enabled', 'true') == 'true'
                 and text('t:Settings/t:MultipleInstancesPolicy') == 'IgnoreNew'
                 and text('t:Settings/t:ExecutionTimeLimit') == 'PT0S'
                 and text('t:Settings/t:AllowHardTerminate') == 'false'
                 and len(task.findall('t:Actions/*', {'t': NS})) == 1
                 and text('t:Actions/t:Exec/t:Command') == config['python']
                 and text('t:Actions/t:Exec/t:Arguments') == watcher_arguments(config)
                 and text('t:Actions/t:Exec/t:WorkingDirectory') == config['cwd'])
        if valid:
            return config
    except (ET.ParseError, ValueError, TypeError, KeyError):
        pass
    raise StartupError('Task ownership/configuration does not match; no changes were made.')


def stop_watcher(state_dir):
    result = run_command([sys.executable, str(WATCHER), '--stop',
                          '--state-dir', str(state_dir)], timeout=280)
    if result.returncode:
        raise StartupError('Watcher stop/cleanup is unconfirmed; the task was preserved.')


def install(sid, port, state_dir, start=False):
    config = make_config(sid, port, state_dir)
    name = task_name(sid)
    old_xml = query_task(name)
    if old_xml is not None:
        old = owned_config(old_xml, sid)
        if old == config:
            if start:
                start_task(name, config)
            print('Logon standby task is already installed and verified' +
                  ('; watcher runtime confirmed.' if start else '.'))
            return 0
        stop_watcher(Path(old['state_dir']))
    # Temporary XML holds paths, not credentials; always remove it after use.
    with tempfile.TemporaryDirectory(prefix='zhaoxi-boot-task-') as temporary:
        xml_path = Path(temporary) / 'task.xml'
        xml_path.write_bytes(task_xml(config))
        command = ['schtasks.exe', '/Create', '/TN', name, '/XML', str(xml_path)]
        if old_xml is not None:
            command.append('/F')
        result = run_command(command)
    if result.returncode:
        raise StartupError('Task installation failed; installed state is unconfirmed.')
    installed = query_task(name)
    if installed is None or owned_config(installed, sid) != config:
        raise StartupError('Task installation could not be verified.')
    if start:
        start_task(name, config)
    print('Logon standby task installed and verified' +
          ('; watcher runtime confirmed.' if start else '.'))
    return 0


def read_watcher(config):
    """Read only the installed task's state; missing/partial files prove nothing."""
    try:
        value = read_status(Path(config['state_dir']) / 'watch.json')
    except (OSError, ValueError, UnicodeError):
        return {}
    return value if isinstance(value, dict) else {}


def watcher_runtime(value, config, now=None):
    """A live PID alone may be reused; also require this instance's heartbeat."""
    now = time.time() if now is None else now
    if (type(value.get('schema')) is not int or value['schema'] != 1 or
            value.get('port') != config['port'] or type(value.get('pid')) is not int or value['pid'] <= 0):
        return False
    if not isinstance(value.get('instance_id'), str) or not re.fullmatch(r'[0-9a-f]{32}', value['instance_id']):
        return False
    started, updated = value.get('started_at'), value.get('updated_at')
    if any(type(stamp) not in (int, float) or not math.isfinite(stamp) for stamp in (started, updated)):
        return False
    if not 0 < started <= updated <= now + 1 or not 0 <= now - updated <= WATCHER_FRESH_SECONDS:
        return False
    return process_running(value['pid']) is True


def watcher_ready(value, config, now=None):
    if not watcher_runtime(value, config, now):
        return False
    phase = value.get('phase')
    if phase in ('waiting-choice', 'polling-wireless'):
        return value.get('serial_open') is True
    if phase == 'wireless-selected':
        return value.get('serial_open') is False and value.get('serial_close_confirmed') is True
    if phase == 'attaching-wired':
        child = value.get('attach_pid')
        return (value.get('serial_open') is False and value.get('serial_close_confirmed') is True
                and type(child) is int and child > 0 and process_running(child) is True)
    return False


def wait_for_watcher(config, requested_at, timeout=WATCHER_START_SECONDS,
                     clock=time.monotonic, wall_clock=time.time, pause=time.sleep):
    """Require a heartbeat published after /Run, including an already-live task."""
    end = clock() + timeout
    while True:
        value = read_watcher(config)
        if (watcher_ready(value, config, wall_clock()) and
                value['updated_at'] >= requested_at):
            return value
        remaining = end - clock()
        if remaining <= 0:
            raise StartupError('Task remains installed, but watcher readiness was not confirmed; '
                               'inspect --status and recover any unconfirmed previous session.')
        pause(min(.2, remaining))


def start_task(name, config):
    requested_at = time.time()
    result = run_command(['schtasks.exe', '/Run', '/TN', name])
    if result.returncode:
        raise StartupError('Task remains installed, but immediate start was not confirmed.')
    return wait_for_watcher(config, requested_at)


def status(sid):
    xml = query_task(task_name(sid))
    if xml is None:
        print('Logon standby task is not installed.')
    else:
        config = owned_config(xml, sid)
        value = read_watcher(config)
        print('Logon standby task is installed and ownership verified.')
        if watcher_ready(value, config):
            messages = {'waiting-choice': 'Watcher is ready and waiting for a network choice.',
                        'polling-wireless': 'Watcher is running; wireless-mode console polling is active.',
                        'wireless-selected': 'Watcher is running; the board selected wireless.',
                        'attaching-wired': 'Watcher is running; its wired attachment process is active.'}
            print(messages[value['phase']])
        elif (value.get('phase') == 'failed' or
              value.get('cleanup_confirmed') is False and not watcher_runtime(value, config)):
            print('Watcher is failed or stopped without confirmed cleanup; explicit recovery is required.')
        elif value.get('phase') == 'stopped' and value.get('cleanup_confirmed') is True:
            print('Watcher is stopped with cleanup confirmed; it is not waiting for the board.')
        elif watcher_runtime(value, config):
            print('Watcher is running, but serial standby readiness is unconfirmed.')
        else:
            print('Watcher runtime is not confirmed; installation alone does not provide wired standby.')
        print('Watcher status does not confirm the board Internet connection.')
    return 0


def stop(sid, state_dir=None):
    xml = query_task(task_name(sid))
    if xml is not None:
        config = owned_config(xml, sid)
        installed_state = Path(config['state_dir'])
        if state_dir is not None and state_dir.resolve() != installed_state:
            raise StartupError('State directory differs from the installed task; stop refused.')
        state_dir = installed_state
    stop_watcher((state_dir or STATE).resolve())
    print('Watcher stop completed with cleanup confirmed by boot_watch.')
    return 0


def uninstall(sid):
    name = task_name(sid)
    xml = query_task(name)
    if xml is None:
        print('Logon standby task is not installed; no task was deleted.')
        return 0
    config = owned_config(xml, sid)
    stop_watcher(Path(config['state_dir']))
    # Re-check after the potentially long cleanup wait; never delete a changed task.
    latest = query_task(name)
    if latest is None or owned_config(latest, sid) != config:
        raise StartupError('Task changed during cleanup; deletion was refused.')
    result = run_command(['schtasks.exe', '/Delete', '/TN', name, '/F'])
    if result.returncode or query_task(name) is not None:
        raise StartupError('Task deletion is unconfirmed; inspect Task Scheduler before retrying.')
    print('Watcher cleanup confirmed; logon standby task removed and absence verified.')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    for action in ('install', 'status', 'uninstall', 'stop'):
        actions.add_argument('--' + action, action='store_true')
    parser.add_argument('--port', help='Required for --install (for example COM20)')
    parser.add_argument('--state-dir', type=Path, help='Watcher state directory; installation defaults to out/pc_link_boot')
    parser.add_argument('--start', action='store_true', help='Immediately run the same installed logon task')
    args = parser.parse_args(argv)
    if os.name != 'nt':
        parser.error('Run this entry on Windows; no administrator privileges are required')
    if args.start and not args.install:
        parser.error('--start requires --install')
    if args.port and not args.install:
        parser.error('--port requires --install')
    if args.state_dir is not None and (args.status or args.uninstall):
        parser.error('--state-dir is only accepted with --install or --stop')
    try:
        sid = current_user_sid()
        if args.install:
            return install(sid, args.port, args.state_dir or STATE, args.start)
        if args.status:
            return status(sid)
        if args.uninstall:
            return uninstall(sid)
        return stop(sid, args.state_dir)
    except StartupError as error:
        print('Boot startup: ' + str(error), file=sys.stderr, flush=True)
        return 1
    except (OSError, ValueError, UnicodeError):
        print('Boot startup: operation failed; state is unconfirmed.', file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

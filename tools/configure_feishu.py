# SPDX-License-Identifier: Apache-2.0
"""Provision the board from the existing local CLI keychain, without logging secrets."""
import argparse
import base64
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import winreg

from local_prompts import open_port
from configure_uplink import Nsh, read_config, require_stopped, CONFIG_DIR, CONFIG

def cli_executable():
    shim = shutil.which('lark-cli.cmd')
    if not shim:
        raise RuntimeError('Install the official @larksuite/cli for this Windows user first')
    binary = Path(shim).parent / 'node_modules/@larksuite/cli/bin/lark-cli.exe'
    if not binary.is_file():
        raise RuntimeError('Official npm native CLI installation was not found')
    return binary


def app_credentials():
    value = json.loads((Path.home() / '.lark-cli/config.json').read_text(encoding='utf-8'))
    apps = value.get('apps', [])
    if len(apps) != 1:
        raise RuntimeError('Select a single CLI application before provisioning')
    app = apps[0]
    ref = app.get('appSecret', {})
    account = 'appsecret:' + app['appId']
    if app.get('brand') != 'feishu' or ref != {'source': 'keychain', 'id': account}:
        raise RuntimeError('Expected a Feishu application backed by its own keychain reference')
    service = 'lark-cli'
    name = base64.urlsafe_b64encode(account.encode()).decode().rstrip('=')
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, 'Software\\LarkCli\\keychain\\' + service) as key:
        encrypted = base64.b64decode(winreg.QueryValueEx(key, name)[0], validate=True)

    class Blob(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('data', ctypes.POINTER(ctypes.c_ubyte))]

    def blob(data):
        buffer = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
        return Blob(len(data), buffer), buffer

    source, source_buffer = blob(encrypted)
    entropy, entropy_buffer = blob((service + '\0' + account).encode())
    result = Blob()
    crypt = ctypes.WinDLL('crypt32', use_last_error=True)
    crypt.CryptUnprotectData.argtypes = [ctypes.POINTER(Blob), ctypes.c_void_p,
        ctypes.POINTER(Blob), ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(Blob)]
    crypt.CryptUnprotectData.restype = wintypes.BOOL
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    if not crypt.CryptUnprotectData(ctypes.byref(source), None, ctypes.byref(entropy),
                                   None, None, 1, ctypes.byref(result)):
        raise RuntimeError('Local CLI keychain decryption failed')
    try:
        secret = ctypes.string_at(result.data, result.size).decode('utf-8')
    finally:
        ctypes.memset(result.data, 0, result.size)
        kernel.LocalFree(result.data)
    return app['appId'], secret


def probe(app_id, secret, mode):
    if mode == 'legacy':
        url = 'https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal'
        body = {'app_id': app_id, 'app_secret': secret}
        token_key = 'tenant_access_token'
    else:
        url = 'https://accounts.feishu.cn/oauth/v3/token'
        body = {'grant_type': 'client_credentials', 'client_id': app_id, 'client_secret': secret}
        token_key = 'access_token'
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
        headers={'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=25) as response:
            status = response.status
            data = json.load(response)
    except urllib.error.HTTPError as error:
        status = error.code
        try:
            data = json.load(error)
        except ValueError:
            data = {}
    token = data.get(token_key)
    return {'mode': mode, 'http': status, 'code': data.get('code'),
        'token_bytes': len(token.encode()) if isinstance(token, str) else 0,
        'compatible': status == 200 and data.get('code') == 0 and
                      isinstance(token, str) and 0 < len(token.encode()) <= 3072}


def apply(app_id, secret, recipient, mode, port_name):
    updates = {'feishu_app_id': app_id, 'feishu_app_secret': secret,
        'feishu_notify_receive_id': recipient, 'feishu_notify_receive_id_type': 'open_id',
        'feishu_notify_auth_mode': mode, 'feishu_notify_enabled': 'true'}
    with open_port(port_name) as port:
        print('BOARD_SETUP stage=console', flush=True)
        nsh = Nsh(port)
        nsh.run('')
        print('BOARD_SETUP stage=stop_agent', flush=True)
        nsh.run('zhaoxi stop', timeout=60)
        end = time.monotonic() + 65
        while True:
            try:
                require_stopped(nsh)
                break
            except RuntimeError:
                if time.monotonic() >= end:
                    raise RuntimeError('Agent shutdown not confirmed; no configuration changed')
                time.sleep(.5)
        print('BOARD_SETUP stage=read_config', flush=True)
        nsh.run('cd ' + CONFIG_DIR)
        previous = read_config(nsh, CONFIG)
        value = dict(previous, **updates)
        payload = json.dumps(value, ensure_ascii=True, separators=(',', ':')).replace("'", r'\u0027')
        if len(payload) > 16384:
            raise RuntimeError('Updated configuration exceeds board limit')
        tmp, backup = 'feishu.new', 'feishu.bak'
        print('BOARD_SETUP stage=write_temporary', flush=True)
        nsh.run("echo -n '' > " + tmp)
        width = 79 - len(("echo -n '' >> " + tmp + '\n').encode())
        for pos in range(0, len(payload), width):
            nsh.run("echo -n '" + payload[pos:pos + width] + "' >> " + tmp)
        if read_config(nsh, tmp) != value:
            raise RuntimeError('Temporary configuration verification failed; original retained')
        nsh.run('cp ' + CONFIG + ' ' + backup)
        if read_config(nsh, backup) != previous:
            raise RuntimeError('On-board backup verification failed; original retained')
        nsh.run('mv ' + tmp + ' ' + CONFIG)
        if read_config(nsh, CONFIG) != value:
            raise RuntimeError('Configuration replacement unverified; on-board backup retained')
        assert all(value[k] == v for k, v in previous.items() if k not in updates)
        # This board's NSH build has no sync command; file operations close
        # their descriptors and are verified by a fresh read before reboot.
        print(json.dumps({'configured': True, 'unrelated_keys_preserved': True,
                          'on_board_backup': CONFIG_DIR + '/' + backup}))
        port.write(b'reboot\n')
        port.flush()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apply', action='store_true')
    p.add_argument('--port', help='Board console port; required with --apply')
    a = p.parse_args()
    if a.apply and not a.port:
        p.error('--apply requires --port')
    app_id, secret = app_credentials()
    status = subprocess.run([str(cli_executable()), 'auth', 'status', '--json', '--verify'],
        capture_output=True, check=True, timeout=30, encoding='utf-8')
    identity = json.loads(status.stdout)
    user = identity['identities']['user']
    if not user.get('verified') or identity['appId'] != app_id:
        raise RuntimeError('Verified receiving account is unavailable')
    target = user['openId']
    if not re.fullmatch(r'ou_[A-Za-z0-9_-]+', target):
        raise RuntimeError('Invalid receiving account')
    results = [probe(app_id, secret, mode) for mode in (('legacy',) if a.apply else ('legacy', 'oauth'))]
    print(json.dumps({'auth_probes': results, 'recipient_name': user.get('userName')}))
    mode = next((row['mode'] for row in results if row['compatible']), None)
    if not mode:
        raise RuntimeError('No board-compatible application authentication mode')
    if a.apply:
        apply(app_id, secret, target, mode, a.port)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Exception arguments can include secrets from JSON/HTTP/serial internals.
        print('Provisioning not confirmed: ' + (str(error) if isinstance(error, RuntimeError) else type(error).__name__))
        sys.exit(1)

#!/usr/bin/env python3
"""Configure saved board uplink over an idle NSH console; never log credentials.

Stop computer forwarding/Agent first. Existing firmware samples uplink_mode at
Agent startup, so restart the board or Agent after changing it. No flashing.
"""
import argparse
import getpass
import json
import os
import re
import secrets
import time
from urllib.parse import urlsplit

CONFIG_DIR = '/emmc/zhaoxi/config'
CONFIG = 'config.json'
TEMP = 'uplink.new'
BACKUP = 'uplink.bak'


class Nsh:
    def __init__(self, port):
        self.port = port

    def run(self, command, timeout=8):
        line = (command + '\n').encode('utf-8')
        if len(line) > 79 or '\n' in command or '\r' in command:
            raise ValueError('Invalid console command length or delimiter')
        # Driver logs can follow a prompt within the same serial read. A
        # separate short NSH acknowledgement is reliable even in that case.
        marker = '__ZX_' + secrets.token_hex(6) + '__'
        wire = line + ('echo ' + marker + '\n').encode()
        for at in range(0, len(wire), 24):
            self.port.write(wire[at:at+24])
            time.sleep(.02)
        end = time.monotonic() + timeout
        output = bytearray()
        while time.monotonic() < end:
            output.extend(self.port.read(8192))
            if len(output) > 65536:
                raise RuntimeError('Console response exceeds limit')
            if re.search(rb'(?:^|[\r\n])' + marker.encode() + rb'[\r\n]', output):
                text = output.decode('utf-8', errors='strict')
                if re.search(r'(?m)^nsh:', text):
                    raise RuntimeError('Board console rejected an operation; current file was not confirmed')
                return text
        # Commands and responses may contain credentials: never include them.
        raise RuntimeError('Board console timed out; inspect board state before retrying')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Configuration contains duplicate JSON keys')
        result[key] = value
    return result


def read_config(nsh, filename):
    text = nsh.run('cat ' + filename)
    start, end = text.find('{'), text.rfind('}')
    if start < 0 or end < start or end-start > 16384:
        raise ValueError('Existing configuration is missing or exceeds limit')
    value = json.loads(text[start:end+1], object_pairs_hook=unique_object)
    if not isinstance(value, dict):
        raise ValueError('Existing configuration must be a JSON object')
    return value


def require_stopped(nsh):
    state = nsh.run('zhaoxi status')
    tasks = nsh.run('ps')
    if 'Agent 消息总线已启动' in state or re.search(r'\bai_agent(?:\s|$)', tasks):
        raise RuntimeError('Stop Agent and wait for shutdown before changing uplink')


def configure(nsh, mode, ssid=None, password=None, wifi_tx_buffer=None, wifi_record_bytes=None, wifi_mss=None, news_static_url=None):
    require_stopped(nsh)
    if mode not in ('wifi', 'pc'):
        raise ValueError('Mode must be wifi or pc')
    if wifi_tx_buffer is not None and (mode != 'wifi' or wifi_tx_buffer not in (0,8192,16384,32768)):
        raise ValueError('Invalid Wi-Fi diagnostic transmit buffer')
    if wifi_record_bytes is not None and (mode!='wifi' or wifi_record_bytes not in (0,512,1024,2048,4096)):
        raise ValueError('Invalid Wi-Fi diagnostic TLS record size')
    if wifi_mss is not None and (mode!='wifi' or wifi_mss not in (0,256,472,512,1024)):
        raise ValueError('Invalid Wi-Fi diagnostic TCP segment size')
    if (ssid is None) != (password is None):
        raise ValueError('SSID and password must be supplied together')
    if ssid is not None:
        if mode != 'wifi' or not 1 <= len(ssid.encode('utf-8')) <= 32:
            raise ValueError('Wi-Fi SSID must contain 1..32 UTF-8 bytes')
        if not 8 <= len(password.encode('utf-8')) <= 63:
            raise ValueError('Wi-Fi password must contain 8..63 UTF-8 bytes')
        if any(c in ssid + password for c in '\x00\r\n'):
            raise ValueError('Wi-Fi settings contain an invalid delimiter')
    if news_static_url is not None:
        parts=urlsplit(news_static_url)
        if (not news_static_url.isascii() or len(news_static_url)>255 or
            any(c.isspace() or ord(c)<32 for c in news_static_url) or
            parts.scheme!='https' or not parts.hostname or parts.username or parts.password or
            parts.query or parts.fragment or not parts.path.endswith('.json')):
            raise ValueError('Static feed requires a public HTTPS JSON URL without credentials or query')
    nsh.run('cd ' + CONFIG_DIR)
    previous = read_config(nsh, CONFIG)
    value = dict(previous)
    if ssid is not None:
        value.update(wifi_ssid=ssid, wifi_pass=password)
    if mode == 'wifi' and (not isinstance(value.get('wifi_ssid'), str) or
                           not value['wifi_ssid'] or not isinstance(value.get('wifi_pass'), str)):
        raise ValueError('No saved Wi-Fi credentials; supply SSID and password')
    value['uplink_mode'] = mode
    if news_static_url is not None:
        value['news_endpoint']=news_static_url
    if wifi_tx_buffer is not None:
        value['wifi_tls_tx_buffer'] = str(wifi_tx_buffer)
    if wifi_record_bytes is not None:
        value['wifi_tls_record_size'] = str(wifi_record_bytes)
    if wifi_mss is not None:
        value['wifi_tcp_mss'] = str(wifi_mss)
    if value == previous:
        return {'changed': False, 'mode': mode, 'verified': True, 'restart_required': False}
    # NSH single quotes preserve JSON escapes. Encode apostrophes as JSON
    # unicode escapes so SSIDs/keys can never break out of a quoted argument.
    payload = json.dumps(value, ensure_ascii=True, separators=(',', ':')).replace("'", r'\u0027')
    if len(payload) > 16384:
        raise ValueError('Updated configuration exceeds limit')
    nsh.run("echo -n '' > " + TEMP)
    overhead = len(("echo -n '' >> " + TEMP + '\n').encode())
    width = 79 - overhead
    for at in range(0, len(payload), width):
        nsh.run("echo -n '" + payload[at:at+width] + "' >> " + TEMP)
    if read_config(nsh, TEMP) != value:
        raise RuntimeError('Temporary configuration verification failed; original retained')
    nsh.run('cp ' + CONFIG + ' ' + BACKUP)
    if read_config(nsh, BACKUP) != previous:
        raise RuntimeError('Backup verification failed; original retained')
    nsh.run('mv ' + TEMP + ' ' + CONFIG)
    if read_config(nsh, CONFIG) != value:
        raise RuntimeError('Configuration replacement was not verified; retain on-board backup')
    return {'changed': True, 'mode': mode, 'verified': True,
            'restart_required': True, 'backup': CONFIG_DIR + '/' + BACKUP}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    action = p.add_mutually_exclusive_group(required=True)
    action.add_argument('--mode', choices=('wifi', 'pc'))
    action.add_argument('--status', action='store_true')
    p.add_argument('--ssid')
    p.add_argument('--news-static-url',help='HTTPS static JSON feed; preserves other runtime configuration')
    p.add_argument('--wifi-tx-buffer',type=int,choices=(0,8192,16384,32768),
                   help='Optional Wi-Fi TLS queue diagnostic; requires SDK patch 0067; 0 keeps default')
    p.add_argument('--wifi-record-bytes',type=int,choices=(0,512,1024,2048,4096),
                   help='Optional Wi-Fi record diagnostic; used only with a nonzero queue option')
    p.add_argument('--wifi-mss',type=int,choices=(0,256,472,512,1024),
                   help='Optional Wi-Fi TCP segment diagnostic; used only with a nonzero queue option')
    a = p.parse_args()
    if a.news_static_url is not None and a.mode is None:p.error('--news-static-url requires --mode')
    if any(value is not None for value in (a.wifi_tx_buffer,a.wifi_record_bytes,a.wifi_mss)) and a.mode!='wifi':
        p.error('Wi-Fi diagnostics require --mode wifi')
    password = None
    if a.ssid is not None:
        if a.mode != 'wifi':p.error('--ssid requires --mode wifi')
        password = os.environ.get('ZHAOXI_WIFI_PASSWORD')
        if password is None:password = getpass.getpass('Hotspot password (hidden): ')
    import serial
    port = serial.Serial();port.port = a.port;port.baudrate = 921600
    port.timeout = .1;port.write_timeout = 2;port.dtr = False;port.rts = False
    try:
        port.open();nsh = Nsh(port);nsh.run('')
        if a.status:
            nsh.run('cd ' + CONFIG_DIR);value = read_config(nsh, CONFIG)
            result = {'mode': 'wifi' if value.get('uplink_mode') == 'wifi' else 'pc',
                      'wifi_ssid_saved': bool(value.get('wifi_ssid')),
                      'wifi_password_saved': bool(value.get('wifi_pass'))}
        else:result = configure(nsh, a.mode, a.ssid, password,a.wifi_tx_buffer,a.wifi_record_bytes,a.wifi_mss,a.news_static_url)
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except UnicodeError:
        print('Uplink configuration not confirmed: invalid console encoding; original configuration/backup retained.')
        return 1
    except (RuntimeError, ValueError) as error:
        print('Uplink configuration not confirmed: '+str(error))
        return 1
    except OSError:
        # Avoid exception text from serial/JSON internals exposing response data.
        print('Uplink configuration not confirmed: serial connection failed; existing on-board configuration/backup retained.')
        return 1
    finally:port.close()


if __name__ == '__main__':raise SystemExit(main())

"""Configuration transfer policy without real serial or private credentials."""
import copy
import json
from pathlib import Path
import re
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from configure_uplink import configure, read_config, Nsh


class Board:
    def __init__(self):
        self.original = {'api_key': 'fixture-only', 'model': 'fixture-model',
                         'custom': {'number': 42}, 'uplink_mode': 'pc'}
        self.files = {'config.json': json.dumps(self.original)}
        self.commands = []
        self.running = self.corrupt_temp = self.fail_backup = False

    def run(self, command):
        self.commands.append(command)
        if command == 'zhaoxi status':return 'Agent 消息总线已启动' if self.running else 'Agent 尚未启动或正在停止'
        if command == 'ps':return '12 ai_agent' if self.running else '8 nsh'
        if command.startswith('cd '):return ''
        if command.startswith('cat '):
            name = command[4:];text = self.files[name]
            if self.corrupt_temp and name == 'uplink.new':text = '{}'
            return command + '\r\n' + text + '\r\nap> '
        if command.startswith('echo '):
            match = re.fullmatch(r"echo -n '([^']*)' (>>|>) uplink.new", command)
            assert match and len(command.encode())+1 <= 79
            self.files['uplink.new'] = (self.files.get('uplink.new', '') if match[2] == '>>' else '') + match[1]
            return ''
        if command == 'cp config.json uplink.bak':
            self.files['uplink.bak'] = '{}' if self.fail_backup else self.files['config.json'];return ''
        if command == 'mv uplink.new config.json':
            self.files['config.json'] = self.files.pop('uplink.new');return ''
        raise AssertionError(command)


class UplinkTests(unittest.TestCase):
    def test_preserve_fields_safe_quotes_backup_and_no_secret_result(self):
        board = Board();ssid = "Campus '$; 热点";password = 'fixture-pa$$word'
        result = configure(board, 'wifi', ssid, password)
        value = json.loads(board.files['config.json'])
        self.assertEqual({k: value[k] for k in board.original if k != 'uplink_mode'},
                         {k: v for k, v in board.original.items() if k != 'uplink_mode'})
        self.assertEqual(value['wifi_ssid'], ssid);self.assertEqual(value['wifi_pass'], password)
        self.assertEqual(json.loads(board.files['uplink.bak']), board.original)
        self.assertNotIn(password, json.dumps(result));self.assertNotIn('fixture-only', json.dumps(result))
        self.assertTrue(result['verified'])

    def test_failed_temp_or_backup_retains_original(self):
        for name in ('corrupt_temp', 'fail_backup'):
            board = Board();old = copy.deepcopy(board.files);setattr(board, name, True)
            with self.assertRaises(RuntimeError):configure(board, 'wifi', 'fixture-ssid', 'fixture-password')
            self.assertEqual(board.files['config.json'], old['config.json'])
            self.assertNotIn('mv uplink.new config.json', board.commands)

    def test_running_agent_and_bad_input_do_not_write(self):
        board = Board();board.running = True
        with self.assertRaises(RuntimeError):configure(board, 'wifi', 'fixture', 'fixture-password')
        self.assertFalse(any(c.startswith(('echo ', 'cp ', 'mv ')) for c in board.commands))
        for ssid, password in [('', 'fixture-password'), ('a'*33, 'fixture-password'),
                               ('fixture', 'short'), ('fixture\n', 'fixture-password')]:
            with self.assertRaises(ValueError):configure(Board(), 'wifi', ssid, password)

    def test_switch_back_preserves_saved_credentials_and_noop(self):
        board = Board();configure(board, 'wifi', 'fixture', 'fixture-password')
        self.assertFalse(configure(board, 'wifi')['changed'])
        configure(board, 'pc');value = json.loads(board.files['config.json'])
        self.assertEqual(value['uplink_mode'], 'pc');self.assertEqual(value['wifi_pass'], 'fixture-password')

    def test_reject_duplicate_keys(self):
        board = Board();board.files['config.json'] = '{"x":1,"x":2}'
        with self.assertRaises(ValueError):read_config(board, 'config.json')

    def test_public_static_feed_preserves_credentials_and_rejects_secret_url(self):
        board=Board();configure(board,'wifi','fixture','fixture-password')
        previous=json.loads(board.files['config.json'])
        configure(board,'wifi',news_static_url='https://example.org/public/candidates.json')
        current=json.loads(board.files['config.json'])
        self.assertEqual(current['news_endpoint'],'https://example.org/public/candidates.json')
        self.assertEqual(current['api_key'],previous['api_key'])
        self.assertEqual(current['wifi_pass'],previous['wifi_pass'])
        for url in ['http://example.org/a.json','https://u:p@example.org/a.json',
                    'https://example.org/a.json?token=private','https://example.org/a.json#part',
                    'https://example.org/a.json\n','https://example.org/a']:
            with self.assertRaises(ValueError):configure(board,'wifi',news_static_url=url)
        self.assertEqual(json.loads(board.files['config.json']),current)

    def test_wifi_buffer_diagnostic_is_explicit_and_bounded(self):
        board=Board();configure(board,'wifi','fixture','fixture-password',8192)
        value=json.loads(board.files['config.json'])
        self.assertEqual(value['wifi_tls_tx_buffer'],'8192')
        configure(board,'wifi',wifi_tx_buffer=0)
        self.assertEqual(json.loads(board.files['config.json'])['wifi_tls_tx_buffer'],'0')
        configure(board,'wifi',wifi_tx_buffer=8192,wifi_record_bytes=512,wifi_mss=472)
        self.assertEqual(json.loads(board.files['config.json'])['wifi_tls_record_size'],'512')
        self.assertEqual(json.loads(board.files['config.json'])['wifi_tcp_mss'],'472')
        for mode,size in [('pc',8192),('wifi',1),('wifi',65536)]:
            with self.assertRaises(ValueError):configure(Board(),mode,wifi_tx_buffer=size)
        with self.assertRaises(ValueError):configure(Board(),'wifi',wifi_record_bytes=4097)
        with self.assertRaises(ValueError):configure(Board(),'wifi',wifi_mss=65536)

    def test_console_prompt_after_json_without_final_newline(self):
        class Port:
            def __init__(self):self.data = [b'cat config.json\r\n{"x":1}ap> \x1b[Kecho __ZX_fixed__\r\n__ZX_fixed__\r\nap> \x1b[K[driver] asynchronous log\r\n'];self.written = []
            def write(self, data):self.written.append(data)
            def read(self, size):return self.data.pop(0) if self.data else b''
        port = Port()
        with patch('configure_uplink.time.sleep'),patch('configure_uplink.secrets.token_hex',return_value='fixed'):
            value = read_config(Nsh(port), 'config.json')
        self.assertEqual(value, {'x': 1});self.assertEqual(b''.join(port.written), b'cat config.json\necho __ZX_fixed__\n')


if __name__ == '__main__':unittest.main()

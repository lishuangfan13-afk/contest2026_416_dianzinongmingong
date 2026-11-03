#!/usr/bin/env python3
"""Prepare fixed public audio assets or install them over an idle NSH console.

Preparation uses the board's existing speech credential in RAM only. No
credential or complete configuration is logged, saved, or put in arguments.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
import time
import zlib
from configure_uplink import Nsh, read_config, require_stopped

PROMPTS = {'hello': '你好',
           'news_ready': '有新的资讯，需要播放吗？请说播放，或暂不播放。',
           'network_error': '网络暂时不可用，请稍后再试。',
           'network_connected': '联网成功'}
HOSTS = {'api.xiaomimimo.com', 'token-plan-cn.xiaomimimo.com',
         'token-plan-sgp.xiaomimimo.com', 'token-plan-ams.xiaomimimo.com'}

def pack(pcm):
    if not pcm or len(pcm)%2 or len(pcm)>480000:
        raise ValueError('PCM must be mono s16le, 1..480000 even bytes')
    return struct.pack('<4sIIHHII', b'ZXP1', 1, 24000, 1, 16, len(pcm), zlib.crc32(pcm))+pcm

def validate(data):
    if len(data)<26:
        raise ValueError('Incomplete prompt')
    magic, version, rate, channels, bits, size, crc = struct.unpack('<4sIIHHII', data[:24])
    if (magic,version,rate,channels,bits)!=(b'ZXP1',1,24000,1,16) or size!=len(data)-24:
        raise ValueError('Invalid prompt header')
    if pack(data[24:])!=data:
        raise ValueError('Invalid prompt CRC or PCM length')
    return size

def open_port(name):
    import serial
    port=serial.Serial();port.port=name;port.baudrate=921600
    port.timeout=.05;port.write_timeout=3;port.dtr=False;port.rts=False
    port.open();return port

def prepare(port, folder, ids=None):
    import requests
    nsh=Nsh(port);nsh.run('cd /emmc/zhaoxi/config')
    config=read_config(nsh,'config.json');key=config['api_key']
    host=config.get('mimo_speech_host') or (
        'token-plan-cn.xiaomimimo.com' if key.startswith(('tp-','ttp-')) else 'api.xiaomimimo.com')
    config=None
    if host not in HOSTS:raise ValueError('Unsupported speech host')
    folder.mkdir(parents=True,exist_ok=True);manifest={'schema':1,'prompts':{}}
    if ids and (folder/'manifest.json').exists():
        manifest=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
        if manifest.get('schema')!=1:raise ValueError('Unknown prompt manifest')
    with requests.Session() as session:
        session.trust_env=False
        for id in ids or PROMPTS:
            text=PROMPTS[id]
            payload={'model':'mimo-v2.5-tts','messages':[{'role':'assistant','content':text}],
                     'audio':{'format':'pcm16','voice':'mimo_default'}}
            with session.post('https://'+host+'/v1/chat/completions',json=payload,
                    headers={'api-key':key},timeout=(10,45),stream=True) as response:
                if response.status_code!=200:raise RuntimeError('Speech request failed: HTTP '+str(response.status_code))
                wire=bytearray()
                for chunk in response.iter_content(8192):
                    wire.extend(chunk)
                    if len(wire)>2000000:raise ValueError('Speech response exceeds byte budget')
            encoded=json.loads(wire)['choices'][0]['message']['audio']['data']
            data=pack(base64.b64decode(encoded,validate=True))
            target=folder/(id+'.zxp');temporary=target.with_suffix('.new')
            temporary.write_bytes(data);temporary.replace(target)
            manifest['prompts'][id]={'text':text,'file':target.name,'size':len(data),
                                      'sha256':hashlib.sha256(data).hexdigest()}
            print(json.dumps({'prepared':id,'bytes':len(data),'audio_ms':(len(data)-24)//48}),flush=True)
    key=None
    (folder/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='')

def wait_line(port, pattern, timeout):
    deadline=time.monotonic()+timeout;wire=bytearray()
    while time.monotonic()<deadline:
        wire.extend(port.read(8192))
        if len(wire)>131072:raise RuntimeError('Console response exceeds budget')
        match=re.search(pattern,wire)
        if match:return match
    raise RuntimeError('Prompt receiver timed out; wait for console recovery')

def install(port, folder, ids=None):
    manifest=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
    assets=[]
    for id in ids or PROMPTS:
        text=PROMPTS[id]
        entry=manifest['prompts'][id]
        if entry['file']!=id+'.zxp' or entry['text']!=text:raise ValueError('Unexpected prompt manifest')
        data=(folder/entry['file']).read_bytes();validate(data)
        if len(data)!=entry['size'] or hashlib.sha256(data).hexdigest()!=entry['sha256']:
            raise ValueError('Prompt asset differs from manifest')
        assets.append((id,data))
    nsh=Nsh(port);require_stopped(nsh)
    for id,data in assets:
        port.write(('zhaoxi prompt receive '+id+' '+str(len(data))+'\n').encode());port.flush()
        wait_line(port,rb'PROMPT_RX_READY bytes='+str(len(data)).encode()+rb' block=1024',8)
        for at in range(0,len(data),1024):
            chunk=data[at:at+1024];port.write(chunk);port.flush()
            expected=str(at+len(chunk)).encode()
            wait_line(port,rb'PROMPT_RX_ACK '+expected+rb'[\r\n]',7)
        # ACK and final RESULT may be delivered together; ask for a framed
        # status afterwards instead of relying on a separate result read.
        status=nsh.run('zhaoxi prompt status',timeout=10)
        match=re.search(re.escape(id)+r' ready=1 bytes=(\d+) rc=0',status)
        if not match or int(match.group(1))!=len(data)-24:raise RuntimeError('Installed prompt readback failed')
        print(json.dumps({'installed':id,'bytes':len(data),'verified':True}),flush=True)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['prepare','install'])
    p.add_argument('--port',required=True);p.add_argument('--assets',type=Path,required=True)
    p.add_argument('--id',dest='ids',action='append',choices=tuple(PROMPTS),help='Prepare/install only the selected fixed prompt')
    a=p.parse_args()
    with open_port(a.port) as port:
        if a.action=='prepare':prepare(port,a.assets,a.ids)
        else:install(port,a.assets,a.ids)

if __name__=='__main__':
    try:main()
    except Exception as error:
        # Server bodies, serial contents and runtime credentials never enter errors.
        print('LOCAL_PROMPTS_FAILED '+type(error).__name__)
        raise SystemExit(1)

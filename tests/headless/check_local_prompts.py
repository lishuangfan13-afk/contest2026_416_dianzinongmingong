#!/usr/bin/env python3
"""Compile actual prompt reader; verify validation-before-play and cancellation."""
from pathlib import Path
import json
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
RUNNER = r'''
#include "local_prompts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static size_t sent;
static int calls, limit, fail;
static int cancel(void *unused){(void)unused;return ++calls>limit;}
static int emit(const unsigned char *p,size_t n,void *unused)
{(void)unused;if(fail)return -EIO;for(size_t i=0;i<n;i++)if(p[i]!=0x12)return -EINVAL;sent+=n;return 0;}
int main(int argc,char **argv)
{
    if(argc!=5)return 2;
    limit=atoi(argv[2]);fail=atoi(argv[3]);bool handled=false;int rc;
    if(!strcmp(argv[4],"status")){char out[256];rc=zhaoxi_prompt_status(out,sizeof(out));puts(out);}
    else rc=zhaoxi_prompt_stream(argv[1],emit,cancel,NULL,&handled);
    printf("RESULT {\"rc\":%d,\"handled\":%s,\"sent\":%zu}\n",rc,handled?"true":"false",sent);
    if(zhaoxi_prompt_id("你好，随便播放") || zhaoxi_prompt_text("../hello"))return 3;
    return 0;
}
'''

def pack(pcm):
    return struct.pack('<4sIIHHII', b'ZXP1', 1, 24000, 1, 16, len(pcm), zlib.crc32(pcm)) + pcm

def main():
    with tempfile.TemporaryDirectory(prefix='zhaoxi-prompts-') as folder:
        temp = Path(folder)
        src = temp/'test.c';src.write_text(RUNNER)
        exe = temp/'test'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-O1',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'app/zhaoxi'), '-DZHAOXI_PROMPT_DIR="'+str(temp)+'"',
            str(ROOT/'app/zhaoxi/local_prompts.c'),str(src),'-o',str(exe)],check=True)
        fixture = temp/'hello.zxp'
        valid = pack(b'\x12'*6400)
        def run(data=valid, id='hello', limit=100, fail=0, mode='stream'):
            if data is not None:fixture.write_bytes(data)
            result = subprocess.run([str(exe),id,str(limit),str(fail),mode],capture_output=True,text=True,check=True,timeout=10)
            return json.loads(next(line[7:] for line in result.stdout.splitlines() if line.startswith('RESULT ')))
        result=run();assert result=={'rc':0,'handled':True,'sent':6400}
        invalid = [b'', valid[:23], valid[:-1], valid+b'x', pack(b'\x12'*3),
                   pack(b''), pack(b'\x12'*480002), b'BAD!'+valid[4:],
                   valid[:8]+struct.pack('<I',16000)+valid[12:],
                   valid[:12]+b'\x02\x00'+valid[14:],
                   valid[:14]+b'\x08\x00'+valid[16:],
                   valid[:20]+b'\0\0\0\0'+valid[24:]]
        for data in invalid:
            result=run(data);assert result['rc']<0 and not result['handled'] and result['sent']==0
        result=run(id='../hello');assert result['rc']<0 and not result['handled']
        fixture.unlink();result=run(data=None);assert result['rc']<0 and not result['handled']
        result=run(limit=0);assert result['rc']<0 and not result['handled'] and result['sent']==0
        result=run(limit=3);assert result['rc']<0 and result['handled'] and result['sent']==3200
        result=run(fail=1);assert result['rc']<0 and result['handled'] and result['sent']==0
        result=run(mode='status');assert result['rc']==0
        print('Local prompt reader: valid, 12 malformed, unknown, missing, validation/playback cancel, output failure, status passed')

if __name__=='__main__':main()

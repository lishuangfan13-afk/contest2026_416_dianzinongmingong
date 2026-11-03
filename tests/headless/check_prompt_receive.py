#!/usr/bin/env python3
"""Use a real pseudo-terminal to exercise raw transfer, restore and atomic save."""
from pathlib import Path
import os
import pty
import select
import struct
import subprocess
import tempfile
import termios
import zlib

ROOT=Path(__file__).resolve().parents[2]
def pack(pcm):return struct.pack('<4sIIHHII',b'ZXP1',1,24000,1,16,len(pcm),zlib.crc32(pcm))+pcm

def main():
    with tempfile.TemporaryDirectory(prefix='zhaoxi-prompt-rx-') as folder:
        temp=Path(folder);audio=temp/'audio';prompts=audio/'prompts'
        master,slave=pty.openpty();saved=termios.tcgetattr(slave)
        runner=temp/'test.c';runner.write_text('''#include "local_prompts.h"
#include <stdlib.h>
int main(int n,char **v){return n==3 && !zhaoxi_prompt_receive(v[1],strtoul(v[2],0,10))?0:1;}
''')
        exe=temp/'test'
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-O1',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(ROOT/'app/zhaoxi'),'-DZHAOXI_AUDIO_DIR="'+str(audio)+'"',
            '-DZHAOXI_PROMPT_CONSOLE="'+os.ttyname(slave)+'"',
            str(ROOT/'app/zhaoxi/local_prompts.c'),str(ROOT/'app/zhaoxi/prompt_receive.c'),
            str(runner),'-o',str(exe)],check=True)
        valid=pack(b'\x12'*6400)
        def line(process, prefix, timeout=8):
            deadline=__import__('time').monotonic()+timeout
            while __import__('time').monotonic()<deadline:
                if not select.select([process.stdout],[],[],.2)[0]:continue
                row=process.stdout.readline()
                if row.startswith(prefix):return row
                if not row:break
            raise RuntimeError('Missing '+prefix.decode())
        def receive(data, interrupt=False):
            process=subprocess.Popen([str(exe),'hello',str(len(valid))],stdout=subprocess.PIPE,bufsize=0)
            line(process,b'PROMPT_RX_READY')
            if interrupt:
                os.write(master,data[:1])
            else:
                for at in range(0,len(data),1024):
                    os.write(master,data[at:at+1024]);line(process,b'PROMPT_RX_ACK ')
            line(process,b'PROMPT_RX_RESULT',timeout=8)
            rc=process.wait(timeout=3);process.stdout.close()
            assert termios.tcgetattr(slave)==saved, 'Console termios not restored'
            assert not (prompts/'hello.part').exists()
            return rc
        assert receive(valid)==0 and (prompts/'hello.zxp').read_bytes()==valid
        bad=valid[:-1]+bytes([valid[-1]^1])
        assert receive(bad)==1 and (prompts/'hello.zxp').read_bytes()==valid
        assert receive(valid,interrupt=True)==1 and (prompts/'hello.zxp').read_bytes()==valid
        assert subprocess.run([str(exe),'../hello','26']).returncode==1
        os.close(master);os.close(slave)
        print('Prompt receiver: PTY upload, bad CRC preserves old asset, partial-upload timeout, console restore and ID restriction passed')

if __name__=='__main__':main()

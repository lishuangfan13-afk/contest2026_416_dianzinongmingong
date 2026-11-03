#!/usr/bin/env python3
"""Create a bounded public feed snapshot; no device persona or model secrets."""
import argparse
import json
from pathlib import Path
import time
from morning_feed import collect

def snapshot(now, collector=collect):
    result=collector(['AI','电子','嵌入式','开源','机器人','半导体'],now)
    body=(json.dumps(result,ensure_ascii=False,separators=(',',':'))+'\n').encode()
    if len(body)>=24000:raise ValueError('Snapshot exceeds board byte budget')
    return body

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();data=snapshot(int(time.time()))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    temporary=a.output.with_suffix('.new');temporary.write_bytes(data);temporary.replace(a.output)
    print(json.dumps({'bytes':len(data),'items':len(json.loads(data)['items'])}))

if __name__=='__main__':main()

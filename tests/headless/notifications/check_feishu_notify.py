#!/usr/bin/env python3
"""Compile the real notification core/bridge and test HTTP on 127.0.0.1 only.

The two SDK boundary declarations are test headers; cJSON and the adapter are
real sources. All credentials, targets, text and server responses are synthetic.
"""
import argparse
import errno
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
TEXT='中文"引号"\\路径\n下一行'
EVENT='synthetic_event_001'
MESSAGE='om_synthetic_message_001'


class Server(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self, scenario):
        super().__init__(('127.0.0.1',0),Handler)
        self.scenario=scenario;self.requests=[];self.errors=[]


class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def do_POST(self):
        try:self.handle_post()
        except (BrokenPipeError,ConnectionResetError):pass
        except BaseException as error:
            self.server.errors.append(type(error).__name__+': '+str(error))
            self.close_connection=True
    def respond(self,status,body,truncate_transport=False):
        payload=body if isinstance(body,bytes) else json.dumps(body,ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(payload)+(10 if truncate_transport else 0)))
        self.send_header('Connection','close');self.end_headers()
        self.wfile.write(payload);self.wfile.flush();self.close_connection=True
    def handle_post(self):
        s=self.server.scenario
        raw=self.rfile.read(int(self.headers['Content-Length']))
        value=json.loads(raw)
        self.server.requests.append({'path':self.path,'value':value})
        auth=self.path in ('/open-apis/auth/v3/tenant_access_token/internal','/oauth/v3/token')
        if auth:
            assert self.headers.get('Authorization') is None,'authorization leaked to auth request'
            oauth=s.startswith('oauth')
            assert self.headers['X-Test-Upstream-Host']==('accounts.feishu.cn' if oauth else 'open.feishu.cn')
            if oauth:
                assert self.path=='/oauth/v3/token'
                assert value=={'grant_type':'client_credentials','client_id':'cli_synthetic_test',
                               'client_secret':'synthetic_test_secret_not_a_credential'}
            else:
                assert value=={'app_id':'cli_synthetic_test','app_secret':'synthetic_test_secret_not_a_credential'}
            if s=='auth_timeout':time.sleep(.4)
            if s=='auth_http':return self.respond(401,{'code':999,'msg':'synthetic denial'})
            if s=='auth_business':return self.respond(200,{'code':10001})
            if s=='auth_negative_code':return self.respond(200,{'code':-1,'tenant_access_token':'synthetic-token'})
            if s=='auth_null_code':return self.respond(200,{'code':None,'tenant_access_token':'synthetic-token'})
            if s=='auth_string_code':return self.respond(200,{'code':'0','tenant_access_token':'synthetic-token'})
            if s=='auth_invalid_json':return self.respond(200,b'{invalid')
            if s=='auth_truncated_json':return self.respond(200,b'{"code":0,"tenant_access_token":"unfinished')
            if s=='auth_transport_truncated':return self.respond(200,b'{"code":0}',True)
            if s=='auth_missing_code':return self.respond(200,{'tenant_access_token':'synthetic-token'})
            if s=='auth_missing_token':return self.respond(200,{'code':0})
            if s=='auth_empty_token':token=''
            elif s=='auth_injected_token':token='synthetic\r\nInjected: yes'
            elif s=='token_3072':token='j'*3072
            elif s=='token_3073':token='j'*3073
            elif s=='token_8193':token='j'*8193
            else:token='synthetic-tenant-token'
            result={'code':0,'access_token' if oauth else 'tenant_access_token':token,'expire':7200}
            if s=='oauth_no_code':result.pop('code')
            return self.respond(200,result)
        expected_type='open_id' if s=='open_id' else 'chat_id'
        assert self.path=='/open-apis/im/v1/messages?receive_id_type='+expected_type
        assert self.headers['X-Test-Upstream-Host']=='open.feishu.cn'
        token='j'*3072 if s=='token_3072' else 'synthetic-tenant-token'
        assert self.headers['Authorization']=='Bearer '+token
        expected_text='朝夕飞书通知链路测试。' if s=='test_entry' else '朝夕提醒：'+TEXT if s=='reminder_entry' else TEXT
        assert value=={'receive_id':'ou_synthetic_target' if s=='open_id' else 'oc_synthetic_target',
                       'msg_type':'text','content':json.dumps({'text':expected_text},ensure_ascii=False,separators=(',',':')),
                       'uuid':EVENT},'outer envelope or content JSON mismatch'
        assert json.loads(value['content'])=={'text':expected_text},'nested text round-trip mismatch'
        if s=='message_timeout':time.sleep(.4)
        if s in ('message_http_401','message_http_429','message_http_503','message_http_201'):
            return self.respond(int(s.rsplit('_',1)[1]),{'code':0,'data':{'message_id':MESSAGE}})
        if s=='message_business':return self.respond(200,{'code':10002})
        if s=='message_invalid_json':return self.respond(200,b'{invalid')
        if s=='message_truncated_json':return self.respond(200,b'{"code":0,"data":{"message_id":"unfinished')
        if s=='message_transport_truncated':return self.respond(200,{'code':0,'data':{'message_id':MESSAGE}},True)
        if s=='message_missing_code':return self.respond(200,{'data':{'message_id':MESSAGE}})
        if s=='message_fraction_code':return self.respond(200,{'code':.5,'data':{'message_id':MESSAGE}})
        if s=='message_boolean_code':return self.respond(200,{'code':False,'data':{'message_id':MESSAGE}})
        if s=='message_negative_code':return self.respond(200,{'code':-1,'data':{'message_id':MESSAGE}})
        if s=='message_null_code':return self.respond(200,{'code':None,'data':{'message_id':MESSAGE}})
        if s=='message_string_code':return self.respond(200,{'code':'0','data':{'message_id':MESSAGE}})
        if s=='message_missing_id':return self.respond(200,{'code':0,'data':{}})
        if s=='message_empty_id':return self.respond(200,{'code':0,'data':{'message_id':''}})
        if s=='message_invalid_id':return self.respond(200,{'code':0,'data':{'message_id':'contains space'}})
        return self.respond(200,{'code':0,'data':{'message_id':MESSAGE}})


def run_case(executable,scenario,success,expected_calls,unknown=None):
    server=Server(scenario);thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.02},daemon=True)
    thread.start()
    try:
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        run=subprocess.run([str(executable),str(server.server_port),scenario],capture_output=True,text=True,
                           timeout=8,env=env)
        assert run.returncode==0,(scenario,'harness/sanitizer failed',run.stderr)
        result=json.loads(run.stdout)
        assert not server.errors,(scenario,server.errors)
        assert result['http_calls']==expected_calls,(scenario,'unexpected HTTP retry count',result)
        assert len(server.requests)==expected_calls,(scenario,'unexpected real loopback request count')
        assert len(result['results'])==(2 if scenario=='repeat' else 1)
        for item in result['results']:
            assert (item['rc']==0)==success,(scenario,'return code',item)
            assert item['accepted']==int(success),(scenario,'acceptance',item)
            assert bool(item['message_id'])==success,(scenario,'false message receipt',item)
            if success:
                assert item['phase']==3 and item['message_id']==MESSAGE and item['api_code']==0
            if unknown is not None:assert item['delivery_unknown']==unknown,(scenario,'delivery certainty',item)
            if expected_calls<=1:assert item['send_attempted']==0,(scenario,'message attempted after auth/config failure')
        if scenario=='repeat':
            messages=[p['value'] for p in server.requests if 'messages?' in p['path']]
            assert len(messages)==2 and messages[0]['uuid']==messages[1]['uuid']==EVENT
        if scenario in ('auth_timeout','message_timeout'):
            assert result['results'][0]['rc']==-errno.ETIMEDOUT
        return expected_calls
    finally:server.shutdown();server.server_close();thread.join(1)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',default='cc');parser.add_argument('--cjson-root',type=Path,required=True)
    args=parser.parse_args()
    cfg_fail=['shutdown','disabled_missing','disabled_false','missing_app','missing_secret','missing_target','unknown_type',
              'chat_id_mismatch','open_id_mismatch','bad_mode','config_no_nul','config_truncated','bad_event','text_too_long']
    success=['success','default_type','default_mode','open_id','oauth','test_entry','reminder_entry','token_3072','repeat']
    auth_fail=['auth_http','auth_business','auth_invalid_json','auth_truncated_json','auth_transport_truncated',
               'auth_missing_code','auth_missing_token','auth_empty_token','auth_injected_token','token_3073','token_8193','auth_timeout',
               'auth_negative_code','auth_null_code','auth_string_code']
    send_fail=['message_http_401','message_http_429','message_http_503','message_http_201','message_business',
               'message_invalid_json','message_truncated_json','message_transport_truncated','message_missing_code',
               'message_fraction_code','message_boolean_code','message_missing_id','message_empty_id','message_invalid_id','message_timeout',
               'message_negative_code','message_null_code','message_string_code']
    with tempfile.TemporaryDirectory(prefix='zhaoxi-feishu-http-') as temp:
        executable=Path(temp)/'feishu_http_harness'
        command=[args.cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
                 '-I'+str(HERE/'include'),'-I'+str(ROOT/'app/zhaoxi/notifications'),'-I'+str(args.cjson_root),
                 str(ROOT/'app/zhaoxi/notifications/feishu_notify.c'),str(ROOT/'app/zhaoxi/notifications/feishu_notify_sdk.c'),
                 str(HERE/'feishu_http_harness.c'),str(args.cjson_root/'cJSON.c'),'-pthread','-lm','-o',str(executable)]
        built=subprocess.run(command,capture_output=True,text=True)
        if built.returncode:raise RuntimeError('Compiler failed:\n'+built.stdout+built.stderr)
        count=requests=0
        for name in cfg_fail:requests+=run_case(executable,name,False,0);count+=1
        for name in success:requests+=run_case(executable,name,True,4 if name=='repeat' else 2,0);count+=1
        for name in auth_fail:requests+=run_case(executable,name,False,1);count+=1
        for name in send_fail:
            uncertain=1 if name in ('message_invalid_json','message_truncated_json','message_transport_truncated',
                                   'message_missing_code','message_fraction_code','message_boolean_code',
                                   'message_negative_code','message_null_code','message_string_code',
                                   'message_missing_id','message_empty_id','message_invalid_id','message_timeout') else None
            requests+=run_case(executable,name,False,2,uncertain);count+=1
    print(json.dumps({'checks':count,'loopback_http_requests':requests,'network_scope':'127.0.0.1 only',
                      'real_sources':['feishu_notify.c','feishu_notify_sdk.c','cJSON.c'],
                      'sanitizers':'ASAN+UBSAN','compiler_warnings':'-Wall -Wextra -Werror',
                      'credentials':'synthetic test data only','board_operations':False},indent=2))


if __name__=='__main__':main()

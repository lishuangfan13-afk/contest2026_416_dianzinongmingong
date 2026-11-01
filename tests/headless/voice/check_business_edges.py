#!/usr/bin/env python3
"""Exercise actual SDK voice dispatch/session helpers with explicit bus/TTS doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def function(source, declaration):
    begin = source.index(declaration)
    return source[begin:source.index('\n}\n', begin) + 3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path)
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    loop = (sdk/'src/core/agent_loop.c').read_text()
    main_source = (sdk/'src/agent_main.c').read_text()
    snippets = '\n'.join(function(loop, name) for name in (
        'static bool automatic_voice_message(',
        'static const char *conversation_id(',
        'static bool automatic_voice_cancelled(',
        'static void push_final_reply(',
        'static void dispatch_response(',
        'static cJSON* build_messages(',
        'static void inject_session_context('))
    snippets += '\n' + '\n'.join(function(main_source, name) for name in (
        'static bool automatic_voice_outbound(',
        'static void automatic_voice_outbound_failed(',
        'static void* outbound_dispatch_task('))
    # Also verify that the actual inbound paths use the common ownership helper.
    body = function(loop, 'static void* agent_loop_task(')
    assert 'build_messages(conversation_id(&msg)' in body
    assert 'push_final_reply(&msg, reply)' in body
    assert 'push_final_reply(&msg, NULL)' in body
    assert 'automatic_voice_cancelled(&msg)' in body
    react = function(loop, 'static char* run_react_loop(')
    assert react.count('automatic_voice_cancelled(msg)') >= 3
    with tempfile.TemporaryDirectory(prefix='zhaoxi-business-voice-') as folder:
        temp = Path(folder)
        source = temp/'test_business_edges.c'
        cjson = sdk.parents[1]/'apps/netutils/cjson/cJSON'
        executable = temp/'test_business_edges'
        common = ['gcc', '-std=gnu11', '-O1', '-g', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'tests/headless/include'),
                        '-I'+str(sdk/'src'), '-I'+str(cjson),
                        str(source), str(cjson/'cJSON.c'), '-o', str(executable)]
        for product in (True, False):
            source.write_text(PREAMBLE + snippets + (TESTS if product else STANDARD_TESTS))
            flags = ['-DCONFIG_EXAMPLES_ZHAOXI=1', '-DCONFIG_ZHAOXI_BES_AUDIO=1'] if product else []
            subprocess.run(common+flags, check=True)
            subprocess.run([str(executable)], check=True, timeout=15)


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <syslog.h>
#include "core/message_bus.h"
#include "voice/voice_channel.h"
#include "cJSON.h"
#define AGENT_CHAN_VOICE "voice"
#define AGENT_CHAN_FEISHU "feishu"
#define AGENT_CHAN_WEBSOCKET "websocket"
#define AGENT_CHAN_SYSTEM "system"
#define AGENT_AI_AGENT_MAX_HISTORY 20
static const char *TAG = "test";
static bool g_shutdown_requested, s_voice_cooldown;
static time_t s_voice_cooldown_until;
static pthread_mutex_t g_stdout_lock = PTHREAD_MUTEX_INITIALIZER;
static agent_msg_t incoming, queued;
static char active_id[64], session_keys[2][64], history_key[64];
static bool active, tap;
static int bus_result, speak_result, pushes, speaks, failed, completed, appends;

bool voice_channel_auto_reply_is_active(const char *id)
{ return active && !strcmp(active_id,id); }
void voice_channel_auto_reply_done_for(const char *id)
{ if (voice_channel_auto_reply_is_active(id)) { active=false;completed++; } }
void voice_channel_auto_reply_failed_for(const char *id,int event)
{ assert(event==VOICE_AUTO_FEEDBACK_DISPATCH_ERROR);
  if (voice_channel_auto_reply_is_active(id)) { active=false;failed++; } }
int message_bus_push_outbound(const agent_msg_t *msg)
{ pushes++;if(bus_result)return bus_result;queued=*msg;return 0; }
int message_bus_pop_outbound(agent_msg_t *msg,uint32_t timeout)
{ (void)timeout;*msg=incoming;g_shutdown_requested=true;return 0; }
bool mbus_tap_try_deliver(const agent_msg_t *msg) { (void)msg;return tap; }
int ws_server_send(const char *id,const char *text) { (void)id;(void)text;return 0; }
int zhaoxi_deliver_reminder(const char *text) { (void)text;return 0; }
int voice_channel_speak(const char *text)
{ assert(text&&text[0]);speaks++;return speak_result; }
int session_append(const char *id,const char *role,const char *text)
{ (void)role;assert(text);assert(appends<2);strcpy(session_keys[appends++],id);return 0; }
int session_get_history_json(const char *id,char *out,size_t capacity,int history)
{ (void)history;strcpy(history_key,id);snprintf(out,capacity,"[]");return 0; }
'''

TESTS = r'''
static agent_msg_t make_message(const char *channel,const char *id,const char *text)
{ agent_msg_t msg={0};strcpy(msg.channel,channel);strcpy(msg.chat_id,id);
  msg.content=text?strdup(text):NULL;return msg; }
static void reset(void)
{ g_shutdown_requested=false;s_voice_cooldown=false;s_voice_cooldown_until=0;
  active=true;strcpy(active_id,"voice_auto_2");tap=false;bus_result=0;speak_result=0;
  pushes=speaks=failed=completed=appends=0;memset(&queued,0,sizeof(queued)); }
static void outbound(const char *channel,const char *id,const char *text)
{ incoming=make_message(channel,id,text);outbound_dispatch_task(NULL); }
int main(void)
{
  reset();outbound("voice","voice_auto_2","reply");
  assert(speaks==1&&completed==1&&failed==0&&!active);
  reset();outbound("voice","voice_auto_1","late");
  assert(speaks==0&&completed==0&&failed==0&&active);
  reset();outbound("voice","voice_auto_2",NULL);
  assert(!speaks&&failed==1&&!active);
  reset();tap=true;outbound("voice","voice_auto_2","intercepted");
  assert(!speaks&&failed==1&&!active);
  reset();s_voice_cooldown=true;s_voice_cooldown_until=time(NULL)+5;
  outbound("voice","voice_auto_2","cooldown");assert(!speaks&&failed==1&&!active);
  reset();speak_result=-1;outbound("voice","voice_auto_2","play failure");
  assert(speaks==1&&failed==1&&completed==0&&!active);
  reset();outbound("reminder","voice_auto_1","future reminder");
  assert(failed==0&&completed==0&&active);
  reset();outbound("voice","manual","manual voice");
  assert(speaks==1&&failed==0&&completed==0&&active);
  reset();agent_msg_t msg=make_message("voice","voice_auto_2","question");
  bus_result=-1;push_final_reply(&msg,strdup("full"));
  assert(pushes==1&&failed==1&&!active);free(msg.content);
  reset();msg=make_message("voice","voice_auto_2","question");
  push_final_reply(&msg,NULL);assert(!pushes&&failed==1&&!active);free(msg.content);
  reset();msg=make_message("voice","voice_auto_1","stale");
  dispatch_response(&msg,strdup("late"));assert(!pushes&&!appends&&active);free(msg.content);
  reset();msg=make_message("voice","voice_auto_2","question");
  dispatch_response(&msg,strdup("answer"));
  assert(!strcmp(session_keys[0],"voice_auto")&&!strcmp(session_keys[1],"voice_auto"));
  assert(!strcmp(queued.chat_id,"voice_auto_2"));free(queued.content);free(msg.content);
  reset();msg=make_message("voice","voice_auto_3","follow-up");
  strcpy(active_id,"voice_auto_3");
  char history[32];cJSON *messages=build_messages(conversation_id(&msg),msg.content,history,sizeof(history));
  assert(!strcmp(history_key,"voice_auto"));cJSON_Delete(messages);free(msg.content);
  reset();msg=make_message("voice","voice_auto_2","question");
  dispatch_response(&msg,NULL);assert(strstr(queued.content,"这次没有处理成功"));
  free(queued.content);free(msg.content);
  char voice[4096]="original\n", cli[4096]="original\n";
  inject_session_context(voice,sizeof(voice),"voice","voice_auto_2");
  inject_session_context(cli,sizeof(cli),"cli","zhaoxi");
  assert(strstr(voice,"通常先说一句")&&strstr(voice,"真实工具"));
  assert(!strstr(cli,"Spoken reply")&&strstr(cli,"Current Session"));
  char tiny[2]={'X','Y'};inject_session_context(tiny,sizeof(tiny),"voice","x");
  assert(tiny[0]=='X'&&tiny[1]=='Y');
  puts("PASS: actual SDK final dispatch, cancellation, queue failure, tap/cooldown/TTS failure, reminder isolation, canonical conversation history and voice-only concise prompt.");
  return 0;
}
'''

STANDARD_TESTS = r'''
int main(void)
{
  agent_msg_t msg={0};strcpy(msg.channel,"voice");strcpy(msg.chat_id,"voice_auto_2");
  msg.content=strdup("question");active=true;strcpy(active_id,msg.chat_id);
  assert(!automatic_voice_message(&msg)&&!automatic_voice_outbound(&msg));
  assert(!automatic_voice_cancelled(&msg));
  assert(!strcmp(conversation_id(&msg),"voice_auto_2"));
  dispatch_response(&msg,strdup("answer"));
  assert(!strcmp(session_keys[0],"voice_auto_2")&&!strcmp(session_keys[1],"voice_auto_2"));
  free(queued.content);
  char history[32];cJSON *messages=build_messages(conversation_id(&msg),msg.content,history,sizeof(history));
  assert(!strcmp(history_key,"voice_auto_2"));cJSON_Delete(messages);
  bus_result=-1;push_final_reply(&msg,strdup("full"));assert(failed==0&&active);
  char prompt[4096]="original\n";inject_session_context(prompt,sizeof(prompt),"voice",msg.chat_id);
  assert(!strstr(prompt,"Spoken reply")&&strstr(prompt,"Current Session"));
  free(msg.content);incoming=(agent_msg_t){0};strcpy(incoming.channel,"voice");
  strcpy(incoming.chat_id,"voice_auto_2");incoming.content=strdup("standard voice");
  outbound_dispatch_task(NULL);assert(speaks==1&&completed==0&&failed==0&&active);
  puts("PASS: standard profile retains per-chat history, original prompt and ordinary voice dispatch without product turn callbacks.");
  return 0;
}
'''


if __name__ == '__main__':
    main()

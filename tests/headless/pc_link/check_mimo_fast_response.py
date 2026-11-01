#!/usr/bin/env python3
"""Exercise actual SDK request builders with a captured HTTP test double."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[3]

def function(source, declaration):
    begin=source.index(declaration)
    return source[begin:source.index('\n}\n',begin)+3]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',type=Path,required=True)
    args=parser.parse_args();sdk=args.sdk.resolve()
    proxy=(sdk/'src/llm/llm_proxy.c').read_text()
    parse=(sdk/'src/llm/llm_parse.c').read_text()
    actual='\n'.join(function(proxy,name) for name in [
        'bool is_openai_compat_host(', 'const char* model_name_for_api(',
        'static void apply_product_mimo_latency(', 'void resp_buf_free(',
        'void extract_text('])
    actual+='\n'+function(parse,'cJSON* build_openai_tools_array(')
    actual+='\n'+function(proxy,'int llm_chat(')
    actual+='\n'+function(proxy,'int llm_chat_tools(')
    with tempfile.TemporaryDirectory(prefix='mimo-fast-response-') as temporary:
        tmp=Path(temporary);inc=tmp/'include';inc.mkdir()
        (inc/'agent_compat.h').write_text('''#pragma once
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#define OK 0
#define ERROR -1
''')
        (inc/'agent_config.h').write_text('''#pragma once
#define AGENT_MAX_TOOL_CALLS 8
#define AGENT_LLM_MAX_TOKENS_OPENAI 16384
#define AGENT_LLM_MAX_TOKENS 4096
#define AGENT_LLM_MAX_RETRIES 0
#define AGENT_LLM_RETRY_BASE_SEC 1
''')
        source=tmp/'test.c';source.write_text(PREAMBLE+actual+TESTS)
        cjson=sdk.parents[1]/'apps/netutils/cjson/cJSON'
        for product in [True,False]:
            executable=tmp/('product' if product else 'standard')
            flags=['-DCONFIG_EXAMPLES_ZHAOXI=1'] if product else []
            subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',*flags,
                '-I'+str(inc),'-I'+str(sdk/'src'),'-I'+str(cjson),str(source),
                str(cjson/'cJSON.c'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=15)

PREAMBLE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include "cJSON.h"
#include "llm/llm_internal.h"
#include "llm/llm_proxy.h"
static const char *TAG="test";
static char s_api_key[128]="test-key-only";
static char s_model[64];
static char s_llm_host[128];
static pthread_mutex_t s_llm_lock=PTHREAD_MUTEX_INITIALIZER;
static cJSON *captured;
static bool tool_result;
static void parse_xml_tool_calls(llm_response_t *r) {(void)r;}
static void parse_ns_xml_tool_calls(llm_response_t *r) {(void)r;}
int llm_http_call(const char *payload,resp_buf_t *rb,int *status)
{
    cJSON_Delete(captured);captured=cJSON_Parse(payload);assert(captured);
    const char *plain="{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"OK\"}}]}";
    const char *tools="{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"call_fixed\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"/test\\\"}\"}}]}}]}";
    rb->data=strdup(tool_result?tools:plain);assert(rb->data);
    rb->len=strlen(rb->data);rb->cap=rb->len+1;*status=200;return OK;
}
'''

TESTS=r'''
static void assert_body(bool wanted,bool tools)
{
#ifndef CONFIG_EXAMPLES_ZHAOXI
    wanted=false;
#endif
    cJSON *thinking=cJSON_GetObjectItem(captured,"thinking");
    assert((thinking!=NULL)==wanted);
    if(wanted)assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(thinking,"type")),"disabled"));
    const char *token_key=is_openai_compat_host(s_llm_host)?"max_completion_tokens":"max_tokens";
    int token_limit=is_openai_compat_host(s_llm_host)?16384:4096;
    assert(cJSON_GetObjectItem(captured,token_key)->valueint==token_limit);
    cJSON *messages=cJSON_GetObjectItem(captured,"messages");assert(cJSON_GetArraySize(messages)==2);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(messages,0),"role")),"system"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(messages,1),"content")),"test input"));
    cJSON *array=cJSON_GetObjectItem(captured,"tools");
    assert((array!=NULL)==tools);
    if(tools){
        cJSON *entry=cJSON_GetArrayItem(array,0);assert(entry);
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(entry,"type")),"function"));
        cJSON *fn=cJSON_GetObjectItem(entry,"function");
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(fn,"name")),"read_file"));
        cJSON *params=cJSON_GetObjectItem(fn,"parameters");assert(cJSON_IsObject(params));
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(params,"type")),"object"));
    }
}
static void check(const char *host,const char *model,bool wanted)
{
    strcpy(s_llm_host,host);strcpy(s_model,model);
    const char *message="[{\"role\":\"user\",\"content\":\"test input\"}]";
    char output[128];tool_result=false;
    assert(llm_chat("test system",message,output,sizeof(output))==OK);
    assert(!strcmp(output,"OK"));assert_body(wanted,false);
    cJSON *messages=cJSON_Parse(message);assert(messages);
    const char *schema="[{\"name\":\"read_file\",\"description\":\"Read existing file\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}]";
    llm_response_t response;tool_result=true;
    assert(llm_chat_tools("test system",messages,schema,&response)==OK);
    assert_body(wanted,true);
    assert(response.tool_use&&response.call_count==1);
    assert(!strcmp(response.calls[0].name,"read_file"));
    cJSON *args=cJSON_Parse(response.calls[0].input);assert(args);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(args,"path")),"/test"));
    cJSON_Delete(args);free(response.text);free(response.reasoning_content);
    for(int i=0;i<response.call_count;i++)free(response.calls[i].input);
    assert(cJSON_GetArraySize(messages)==1);cJSON_Delete(messages);
}
int main(void)
{
    const char *official[]={"api.xiaomimimo.com","token-plan-cn.xiaomimimo.com","token-plan-sgp.xiaomimimo.com","token-plan-ams.xiaomimimo.com"};
    for(unsigned i=0;i<sizeof(official)/sizeof(official[0]);i++){
        check(official[i],"mimo-v2.6-flash",true);
        check(official[i],"mimo-v2.6-pro",false);
        check(official[i],"mimo-v2.5-tts",false);
    }
    check("api.openai.com","mimo-v2.6-flash",false);
    check("openrouter.ai","mimo-v2.6-flash",false);
    check("other.example","mimo-v2.6-flash",false);
    check("api.xiaomimimo.com.example","mimo-v2.6-flash",false);
    check("api.xiaomimimo.com","mimo-v2.6-flash-extra",false);
    cJSON_Delete(captured);
    puts("PASS: both actual request builders preserve tools/history/token limits; fast mode limited to product official MiMo flash.");
    return 0;
}
'''

if __name__=='__main__':main()

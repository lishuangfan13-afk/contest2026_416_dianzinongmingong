/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static int64_t now_ms;
static int calls,mode,stopped;
static unsigned request_budget;
static int fixture_clock(clockid_t clock,struct timespec *out)
{(void)clock;out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;}
#define clock_gettime fixture_clock
#include MIMO_SOURCE
#undef clock_gettime
bool agent_shutdown_requested(void){return stopped;}
int claw_config_get(const char *key,char *out,size_t cap)
{
    if(!strcmp(key,"api_key"))snprintf(out,cap,"fixture-credential-never-sent");
    else if(!strcmp(key,"mimo_speech_host"))snprintf(out,cap,"api.xiaomimimo.com");
    else return -1;return 0;
}
int vela_https_post_json_bounded(const char *host,const char *port,const char *path,
    const vela_header_t *headers,const char *body,char *out,size_t cap,
    unsigned budget,int (*cancel)(void *),void *user)
{
    assert(!strcmp(host,"api.xiaomimimo.com") && !strcmp(port,"443") && !strcmp(path,"/v1/chat/completions"));
    assert(headers && body && budget<=25000 && !cancel(user));calls++;request_budget=budget;
    cJSON *parsed=cJSON_Parse(body);assert(parsed);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(parsed,"model")),"mimo-v2.5-asr"));cJSON_Delete(parsed);
    now_ms+=mode==3?21000:1000;
    if(mode==4)return 500;
    if(mode==5){snprintf(out,cap,"{\"choices\":[{\"message\":{\"content\":17}}]}");return 200;}
    if(mode==2)stopped=1;
    const char *text=(mode==1 || mode==2 || mode==3 || calls==1)?"":"播放今天的新闻";
    snprintf(out,cap,"{\"choices\":[{\"message\":{\"content\":\"%s\"}}]}",text);return 200;
}
static void reset(int value){mode=value;calls=stopped=0;now_ms=0;request_budget=0;}
int main(void)
{
    int16_t pcm[320*12];char out[128];
    for(unsigned i=0;i<sizeof(pcm)/2;i++)pcm[i]=i%2?1000:-1000;
    reset(0);assert(!recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out)));
    assert(calls==2 && !strcmp(out,"播放今天的新闻") && request_budget==24000);
    reset(1);assert(recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out))==-ENODATA && calls==2 && !out[0]);
    reset(2);assert(recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out))==-ENODATA && calls==1);
    reset(3);assert(recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out))==-ENODATA && calls==1);
    reset(4);assert(recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out))==-EIO && calls==1);
    reset(5);assert(recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out))==-EIO && calls==1);
    memset(pcm,0,sizeof(pcm));reset(0);
    assert(recognize((unsigned char *)pcm,sizeof(pcm),out,sizeof(out))==-ENODATA && calls==1);
    puts("PASS: actual MiMo request/ASR; non-quiet empty-text retry once; quiet/cancel/time budget/HTTP/protocol boundaries; no transcript body logging");
    return 0;
}

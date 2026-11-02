/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "morning.h"
#include "cJSON.h"
static atomic_int shutting_down, enabled, context_on, outputs, prompts, stories, resumed;
static atomic_int fail_output, blocked_output, output_entered;
static int64_t wall=1793574000LL;
static pthread_t worker;
static time_t fixture_time(time_t *out){if(out)*out=(time_t)wall;return (time_t)wall;}
static int fixture_settimeofday(const struct timeval *v,const struct timezone *z)
{(void)z;wall=v->tv_sec;return 0;}
static int fixture_usleep(useconds_t amount){(void)amount;return usleep(2000);}
#define time fixture_time
#define settimeofday fixture_settimeofday
#define usleep fixture_usleep
#include MORNING_SOURCE
#undef time
#undef settimeofday
#undef usleep

bool agent_shutdown_requested(void){return atomic_load(&shutting_down);}
int agent_task_create(void *(*f)(void *),const char *name,int stack,void *arg,int priority)
{(void)name;(void)stack;(void)priority;return pthread_create(&worker,NULL,f,arg);}
bool network_is_connected(void){return true;}
int claw_config_get(const char *key,char *out,size_t cap)
{if(strcmp(key,"news_endpoint"))return -1;snprintf(out,cap,"http://127.0.0.1:8766/candidates");return 0;}
int claw_config_set(const char *key,const char *value){(void)key;(void)value;return 0;}
int morning_endpoint_valid(const char *s){return s?0:-EINVAL;}
int morning_sntp(int64_t *epoch){*epoch=wall;return 0;}
int morning_fetch_candidates(const char *url,const char *profile,int64_t now,char *out,size_t cap)
{(void)url;(void)profile;(void)now;snprintf(out,cap,"{}");return 0;}
int morning_summarize(const char *profile,const char *candidates,int64_t now,char *out,size_t cap)
{
    (void)profile;(void)candidates;
    snprintf(out,cap,"{\"schema\":1,\"items\":[{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"title\":\"测试资料\",\"source\":\"Test fixture\",\"url\":\"https://example.org/test\",\"published_at\":%lld,\"summary\":\"仅用于许可状态机测试，不是真实新闻。\",\"relevance\":\"用于验证。\"}]}",(long long)(now-60));return 0;
}
int voice_channel_auto_status(char *out,size_t cap)
{snprintf(out,cap,"{\"enabled\":%s,\"running\":%s}",atomic_load(&enabled)?"true":"false",atomic_load(&enabled)?"true":"false");return 0;}
int voice_channel_auto_output_begin(bool idle_only)
{(void)idle_only;if(!atomic_load(&enabled) || atomic_exchange(&outputs,1))return -EBUSY;return 0;}
void voice_channel_auto_output_end(bool conversation)
{assert(atomic_exchange(&outputs,0));if(conversation)atomic_fetch_add(&resumed,1);}
int voice_channel_auto_news_consent(bool on){atomic_store(&context_on,on && atomic_load(&enabled));return 0;}
int voice_channel_speak_auto_owned(const char *text)
{
    assert(atomic_load(&outputs));
    if(strstr(text,"需要播放吗"))atomic_fetch_add(&prompts,1);else atomic_fetch_add(&stories,1);
    atomic_store(&output_entered,1);
    while(atomic_load(&blocked_output) && !agent_shutdown_requested())usleep(1000);
    return agent_shutdown_requested()?-ECANCELED:atomic_load(&fail_output)?-EIO:0;
}
static void wait_phase(enum phase expected)
{
    for(int i=0;i<5000;i++){
        pthread_mutex_lock(&state.lock);bool yes=state.phase==expected;pthread_mutex_unlock(&state.lock);
        if(yes)return;usleep(1000);
    }
    assert(!"phase timeout");
}
static void ask(void)
{char *argv[]={"zhaoxi","news","ask"};assert(!zhaoxi_morning_command(3,argv));wait_phase(PERMISSION);}
static void stop(void)
{atomic_store(&shutting_down,1);pthread_join(worker,NULL);assert(!state.running && !atomic_load(&outputs) && !atomic_load(&context_on));}
int main(void)
{
    atomic_store(&enabled,1);assert(!morning_profile_set("热爱AI、电子的大学生。"));
    assert(!zhaoxi_morning_start());wait_phase(PERMISSION);
    assert(atomic_load(&prompts)==1 && !atomic_load(&stories) && atomic_load(&context_on));
    assert(zhaoxi_morning_voice("不要播放新闻。",false)==1);wait_phase(IDLE);
    char day[11];assert(!morning_day(wall,day,NULL));struct morning_batch *batch=calloc(1,sizeof(*batch));
    for(int i=0;i<100 && (!morning_batch_load(day,batch) && !batch->declined);i++)usleep(1000);
    assert(batch->declined && batch->asked && !batch->played);assert(!atomic_load(&stories));
    stop();atomic_store(&shutting_down,0);assert(!zhaoxi_morning_start());usleep(50000);
    assert(atomic_load(&prompts)==1); /* Restart cannot repeat an announced batch. */
    ask();assert(zhaoxi_morning_voice("你说要听是什么意思",false)==1);
    assert(!atomic_load(&stories));
    pthread_mutex_lock(&state.lock);state.consent_until=ticks()-1;pthread_mutex_unlock(&state.lock);
    wait_phase(IDLE);assert(!atomic_load(&context_on) && !atomic_load(&stories));
    assert(zhaoxi_morning_voice("播放",false)==0); /* Late yes is inert. */
    ask();assert(zhaoxi_morning_voice("播放新闻。",false)==1);
    for(int i=0;i<5000 && !atomic_load(&resumed);i++)usleep(1000);
    assert(atomic_load(&stories)==1 && atomic_load(&resumed)==1);
    wait_phase(IDLE);assert(!morning_batch_load(day,batch) && batch->played==1 && !batch->declined);
    ask();assert(zhaoxi_morning_voice("你好，夕夕，今天几号",false)==0);assert(state.phase==IDLE && !atomic_load(&context_on));
    assert(!zhaoxi_morning_voice("播放今天的新闻。",false));
    assert(zhaoxi_morning_voice("不要播放今天的新闻。",true)==0);
    assert(zhaoxi_morning_voice(" 播放今天的新闻。 ",true)==1);
    for(int i=0;i<5000 && atomic_load(&stories)<2;i++)usleep(1000);
    wait_phase(IDLE);assert(atomic_load(&stories)==2);
    atomic_store(&blocked_output,1);atomic_store(&output_entered,0);
    char *argv[]={"zhaoxi","news","ask"};assert(!zhaoxi_morning_command(3,argv));
    for(int i=0;i<5000 && !atomic_load(&output_entered);i++)usleep(1000);
    assert(atomic_load(&output_entered));stop();free(batch);
    puts("PASS: real service threads fetch/save before asking; explicit denial and silence never play; restart does not replay; permission is batch-bound; late yes ignored; fresh wake preempts; shutdown cancels announcement and releases output");
    return 0;
}

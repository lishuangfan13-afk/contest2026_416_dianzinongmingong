/* SPDX-License-Identifier: Apache-2.0 */
/* Actual service/storage. Reuse explicit cloud/audio substitutes from the
 * consent regression, exercising different calendar days and request races. */
#define main consent_regression_main
#include "test_morning_runtime.c"
#undef main
static void wait_fetch(int expected)
{
    for(int i=0;i<5000 && atomic_load(&fetch_calls)<expected;i++)usleep(1000);
    assert(atomic_load(&fetch_calls)==expected);wait_phase(IDLE);
}
static void request(const char *text)
{assert(zhaoxi_morning_voice(text,true)==1);}
static void wait_request(void)
{
    for(int i=0;i<5000;i++) {
        pthread_mutex_lock(&state.lock);bool done=!state.play_requested && state.phase==IDLE;pthread_mutex_unlock(&state.lock);
        if(done)return;usleep(1000);
    }
    assert(!"playback request did not finish");
}
static void restart(void)
{atomic_store(&shutting_down,0);assert(!zhaoxi_morning_start());}
int main(void)
{
    atomic_store(&enabled,1);atomic_store(&online,1);atomic_store(&user_busy,1);
    state.boot_network_announced=true;wall+=86400;
    assert(!morning_profile_set("离线新闻状态机测试"));
    char json[MORNING_JSON_MAX],today[11],yesterday[11];
    struct morning_batch *b=calloc(1,sizeof(*b));assert(b);
    assert(!morning_day(wall,today,NULL));assert(!morning_day(wall-86400,yesterday,NULL));
    assert(!morning_summarize("fixture","{}",wall-86400,json,sizeof(json)));
    assert(!morning_batch_parse(json,wall-86400,b));memset(b->items[0].id,'b',64);
    strcpy(b->items[0].summary,"昨天保存的测试摘要");
    assert(!morning_batch_render(b,json,sizeof(json)));assert(!morning_batch_parse(json,wall-86400,b));
    b->played=1;b->asked=true;assert(!morning_batch_save(b));
    assert(!morning_batch_parse("{\"schema\":1,\"items\":[]}",wall,b));assert(!morning_batch_publish(b));
    atomic_store(&reserve_busy,1);restart();request("请帮我播放一下今天的资讯吧");usleep(20000);
    assert(state.play_requested && !atomic_load(&fetch_calls));
    atomic_store(&reserve_busy,0);wait_request();assert(atomic_load(&fetch_calls)==1);
    assert(!morning_batch_load(today,b) && b->count==1 && b->played==1);
    assert(strstr(last_spoken,"下面播报今天的新闻"));
    assert(!morning_batch_load(yesterday,b) && b->items[0].id[0]=='b');
    int calls=atomic_load(&fetch_calls);request("我想听昨天的新闻");wait_request();
    assert(atomic_load(&fetch_calls)==calls && strstr(last_spoken,"下面播报昨天保存的新闻") && strstr(last_spoken,"昨天保存的测试摘要"));
    /* A dated request also supersedes the automatic permission question. */
    atomic_store(&user_busy,0);ask();assert(zhaoxi_morning_voice("播报一下昨天的新闻",false)==1);wait_request();
    assert(!atomic_load(&context_on) && strstr(last_spoken,"昨天保存的测试摘要"));atomic_store(&user_busy,1);
    char *fetch[]={"zhaoxi","news","fetch"};assert(!zhaoxi_morning_command(3,fetch));wait_fetch(calls+1);
    assert(!morning_batch_load(today,b) && b->count==1 && b->played==1); /* No duplicate or rewind. */
    assert(!morning_batch_load(today,b));memset(b->items[0].id,'c',64);
    assert(!morning_batch_refresh(b));assert(!morning_batch_load(today,b) && b->count==2 && b->played==1);
    atomic_store(&fail_output,1);request("播放今天的新闻");wait_request();
    assert(!morning_batch_load(today,b) && b->played==1);
    atomic_store(&fail_output,0);request("把今天的新闻播放一下");wait_request();
    assert(!morning_batch_load(today,b) && b->played==2 && strstr(last_spoken,"第2条"));
    stop();wall-=2*86400;restart();request("播放昨天的新闻");wait_request();
    assert(strstr(last_spoken,"还没有保存昨天的新闻"));
    atomic_store(&empty_summary,1);request("播放今天的新闻");wait_request();
    assert(strstr(last_spoken,"今天暂时没有新的新闻"));
    assert(!morning_day(wall,today,NULL));assert(!morning_batch_load(today,b) && !b->count);
    calls=atomic_load(&fetch_calls);atomic_store(&empty_summary,0);
    assert(!zhaoxi_morning_command(3,fetch));wait_fetch(calls+1);
    assert(!morning_batch_load(today,b) && b->count==1); /* Existing empty file is refreshed. */
    stop();wall+=5*86400;atomic_store(&fetch_error,-EIO);restart();request("播放今天的新闻");wait_request();
    assert(strstr(last_spoken,"今天的新闻获取失败"));
    atomic_store(&user_busy,0);
    for(int i=0;i<4;i++) {
        calls=atomic_load(&fetch_calls);pthread_mutex_lock(&state.lock);state.next_try=0;pthread_mutex_unlock(&state.lock);
        wait_fetch(calls+1);
    }
    assert(state.attempt>=5); /* Recovery is never exhausted after three failures. */
    atomic_store(&user_busy,1);atomic_store(&fetch_error,0);calls=atomic_load(&fetch_calls);
    assert(!zhaoxi_morning_command(3,fetch));wait_fetch(calls+1);
    assert(state.attempt==0);stop();free(b);
    puts("PASS: today/yesterday routing; pending busy request; foreground fetch; empty refresh; manual refresh; calendar archive isolation; explicit no-data/failure replies; resume after playback failure; retries beyond three; fresh permission selection");
    return 0;
}

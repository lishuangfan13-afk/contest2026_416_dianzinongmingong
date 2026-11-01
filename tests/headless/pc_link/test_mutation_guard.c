/* SPDX-License-Identifier: Apache-2.0 */
/* Actual ReAct loop, assistant/tool messages and file tools; only LLM, router,
 * trace and cache are fixtures. No board, microphone or external service. */
#define AGENT_MAX_TOOL_CALLS 4
#define AGENT_AI_AGENT_MAX_TOOL_ITER 6
#define AGENT_LLM_TIMEOUT_SEC 35
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "core/agent_trace.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "infra/cron_service.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#define syslog(...) ((void)0)
#define TOOL_OUTPUT_SIZE_LARGE 16384
#define TOOL_OUTPUT_SIZE_MIN 2048
#define LLM_TIMEOUT_MSG "timeout"
#define LLM_TIMEOUT_TASK_COMPLETE_MSG "timeout-complete"
typedef struct { char *content; char channel[16]; char chat_id[64]; } agent_msg_t;
typedef struct { size_t buf_size; } pool_t;
static pool_t s_tool_pool;
static bool s_pool_ready;
static const char *TAG = "guard-test";
static unsigned model_calls, cache_gets, cache_puts;
static atomic_uint tool_calls;
static int scenario;
static bool duplicate_exit, poison_cache;
static bool fail_edit_write;
int agent_task_create(void *(*fn)(void *),const char *name,int stack,void *arg,int priority)
{ (void)fn;(void)name;(void)stack;(void)arg;(void)priority;return 0; }
int message_bus_push_outbound(const void *message) { (void)message;return -1; }
FILE *__real_fopen(const char *path,const char *mode);
FILE *__wrap_fopen(const char *path,const char *mode)
{
    if(fail_edit_write && !strcmp(path,AGENT_DATA_DIR "/full.md") && !strcmp(mode,"w"))
        return __real_fopen("/dev/full",mode);
    return __real_fopen(path,mode);
}
static uint32_t calc_elapsed_ms(const struct timeval *a,const struct timeval *b) { (void)a;(void)b;return 1; }
static bool automatic_voice_cancelled(const agent_msg_t *m) { (void)m;return false; }
static bool llm_call_timed_out(uint32_t ms) { (void)ms;return false; }
static void send_working_status(const agent_msg_t *m,int i) { (void)m;(void)i; }
static size_t agent_mem_safe_size(size_t preferred,size_t minimum) { (void)minimum;return preferred; }
static char *agent_pool_acquire(pool_t *p) { (void)p;return NULL; }
static void agent_pool_release(pool_t *p,void *v) { (void)p;free(v); }
static char *inject_cron_context(const char *n,const char *i,const char *c,const char *id) { (void)n;(void)i;(void)c;(void)id;return NULL; }
static int agent_tool_exec_streamed(const char *n,const char *i,char *o,size_t z)
{
    atomic_fetch_add(&tool_calls,1);
    if (!strcmp(n,"write_file")) tool_write_file_execute(i,o,z);
    else if (!strcmp(n,"edit_file")) tool_edit_file_execute(i,o,z);
    else if (!strcmp(n,"read_file")) tool_read_file_execute(i,o,z);
    else if (!strcmp(n,"cron_add")) tool_cron_add_execute(i,o,z);
    else if (!strcmp(n,"cron_remove")) tool_cron_remove_execute(i,o,z);
    else if (!strcmp(n,"cron_list")) tool_cron_list_execute(i,o,z);
    else assert(0);
    return (int)strlen(o);
}
int tool_registry_execute(const char *n,const char *i,char *o,size_t z)
{ return agent_tool_exec_streamed(n,i,o,z); }
llm_complexity_t llm_router_estimate_complexity(const char *p,size_t z) { (void)p;(void)z;return LLM_COMPLEXITY_SIMPLE; }
int llm_router_select(llm_complexity_t c) { (void)c;return -1; }
int llm_router_select_with_profile(llm_complexity_t c,llm_route_profile_t p) { (void)c;(void)p;return -1; }
int llm_router_apply(int i) { (void)i;return 0; }
llm_route_profile_t llm_router_get_profile(void) { return LLM_ROUTE_ECO; }
void llm_router_report_failure(int i) { (void)i; }
void llm_router_report_success(int i) { (void)i; }
void llm_router_report_latency(int i,uint32_t m) { (void)i;(void)m; }
void llm_router_report_tokens(int i,int p,int c) { (void)i;(void)p;(void)c; }
void agent_trace_begin(agent_trace_t *t,const char *id,const char *channel) { (void)id;(void)channel;memset(t,0,sizeof(*t)); }
void agent_trace_step(agent_trace_t *t,int i,const char *n,uint32_t m,int ok) { (void)t;(void)i;(void)n;(void)m;(void)ok; }
void agent_trace_end(agent_trace_t *t,int status) { t->status=status; }
static char *llm_cache_get(const char *p,size_t n) { (void)p;(void)n;cache_gets++;return poison_cache?strdup("cached-success"):NULL; }
static void llm_cache_put(const char *p,size_t n,const char *r) { (void)p;(void)n;(void)r;cache_puts++; }
static void llm_cache_put_tokens(const char *p,size_t n,int t) { (void)p;(void)n;(void)t; }
static bool check_tool_dup(const llm_response_t *r,char *s,int *d,char *n,int *c) { (void)r;(void)s;(void)d;(void)n;(void)c;return duplicate_exit; }
static char *force_finish_reply(const char *p,cJSON *m) { (void)p;(void)m;return strdup("已添加，编号2。"); }
static char *handle_task_complete(const char *p,cJSON *m,bool *timeout) { (void)p;(void)m;*timeout=false;return strdup("已完成。"); }
void llm_response_free(llm_response_t *r)
{ free(r->text);free(r->reasoning_content);for(int i=0;i<r->call_count;i++)free(r->calls[i].input);memset(r,0,sizeof(*r)); }
static void model_tool(llm_response_t *r,int index,const char *name,const char *path,const char *content)
{
    llm_tool_call_t *c=&r->calls[index];
    snprintf(c->id,sizeof(c->id),"call-%u-%d",model_calls,index);
    snprintf(c->name,sizeof(c->name),"%s",name);
    cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"path",path);
    if(!strcmp(name,"edit_file")) {
        cJSON_AddStringToObject(a,"old_string","old");cJSON_AddStringToObject(a,"new_string","new");
    } else if(content)cJSON_AddStringToObject(a,"content",content);
    c->input=cJSON_PrintUnformatted(a);c->input_len=strlen(c->input);cJSON_Delete(a);
    r->call_count=index+1;r->tool_use=true;
}
static void model_cron(llm_response_t *r,const char *name,bool fail)
{
    llm_tool_call_t *c=&r->calls[0];snprintf(c->id,sizeof(c->id),"cron-%u",model_calls);
    snprintf(c->name,sizeof(c->name),"%s",name);cJSON *a=cJSON_CreateObject();
    if(!strcmp(name,"cron_add")) {
        cJSON_AddStringToObject(a,"name","guard-test");cJSON_AddStringToObject(a,"schedule_type","every");
        cJSON_AddNumberToObject(a,"interval_s",fail?0:60);cJSON_AddStringToObject(a,"message","real reminder");
        cJSON_AddStringToObject(a,"channel","voice");cJSON_AddStringToObject(a,"chat_id","zhaoxi");
    } else if(!strcmp(name,"cron_remove")) {
        cron_job_t jobs[AGENT_CRON_MAX_JOBS];int count=cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS);
        cJSON_AddStringToObject(a,"job_id",!fail&&count?jobs[0].id:"ffffffff");
    }
    c->input=cJSON_PrintUnformatted(a);c->input_len=strlen(c->input);cJSON_Delete(a);r->call_count=1;r->tool_use=true;
}
int llm_chat_tools(const char *p,cJSON *m,const char *tools,llm_response_t *r)
{
    (void)p;(void)m;(void)tools;memset(r,0,sizeof(*r));unsigned n=model_calls++;
    const char *tasks=AGENT_DATA_DIR "/TASKS.md";
    if(scenario==1 && n==0) model_tool(r,0,"read_file",tasks,NULL);
    else if(scenario==1 && n==2) model_tool(r,0,"write_file",tasks,"- [ ] [1] old\n- [ ] [2] new\n");
    else if(scenario==2 && (n==0 || n==2)) model_tool(r,0,"write_file",tasks,NULL);
    else if(scenario==3 && (n==0 || n==2)) model_tool(r,0,"write_file",AGENT_DATA_DIR "/OTHER.md","new");
    else if(scenario==4 && n==0) {
        model_tool(r,0,"read_file",AGENT_DATA_DIR "/OTHER.md",NULL);
        model_tool(r,1,"write_file",tasks,"parallel-save");
    } else if(scenario==5 && n==0) model_tool(r,0,"write_file",AGENT_DATA_DIR "/memory/daily/2026-11-01.md","real-note");
    else if(scenario==6 || scenario==7 || ((scenario==8 || scenario==9) && n==0)) model_tool(r,0,"read_file",tasks,NULL);
    else if(scenario==10 && n==0) model_tool(r,0,"edit_file",tasks,NULL);
    else if(scenario==11 && n==0) model_cron(r,"cron_add",false);
    else if(scenario==12 && n==0) model_cron(r,"cron_remove",false);
    else if(scenario==13 && (n==0||n==2)) model_tool(r,0,"write_file",tasks,"wrong proof for a reminder");
    else if(scenario==14 && n==0) model_cron(r,"cron_list",false);
    else if(scenario==16 && (n==0||n==2)) model_cron(r,"cron_add",true);
    else if(scenario==17 && (n==0||n==2)) model_cron(r,"cron_remove",true);
    else { r->text=strdup("已添加，编号2。");r->text_len=strlen(r->text); }
    return 0;
}

/* ACTUAL_ASSISTANT_MESSAGES */
/* ACTUAL_TOOL_MESSAGES */
/* ACTUAL_MUTATION_HELPERS */
/* ACTUAL_REACT_LOOP */

#ifndef PRODUCT_MUTATION_UNCONFIRMED
#define PRODUCT_MUTATION_UNCONFIRMED "unconfirmed"
#endif

static void write_text(const char *path,const char *text)
{ FILE *f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(fclose(f)==0); }
static char *run_case(const char *request,int mode,bool history)
{
    model_calls=cache_gets=cache_puts=0;atomic_store(&tool_calls,0);scenario=mode;
    write_text(AGENT_DATA_DIR "/TASKS.md",mode==8?"TASK_COMPLETE":mode==9?"OK: wrote 3 bytes":"old");
    cJSON *messages=cJSON_CreateArray();
    if(history){
        llm_response_t old={0};model_tool(&old,0,"write_file",AGENT_DATA_DIR "/TASKS.md","old");
        add_assistant_message(messages,&old);
        cJSON *result=cJSON_CreateObject();cJSON_AddStringToObject(result,"role","tool");
        cJSON_AddStringToObject(result,"tool_call_id",old.calls[0].id);
        cJSON_AddStringToObject(result,"content","OK: wrote 3 bytes");cJSON_AddItemToArray(messages,result);llm_response_free(&old);
    }
    cJSON *u=cJSON_CreateObject();cJSON_AddStringToObject(u,"role","user");cJSON_AddStringToObject(u,"content",request);cJSON_AddItemToArray(messages,u);
    agent_msg_t msg={.content=(char*)request,.channel="voice",.chat_id="zhaoxi"};char output[8192];
    char *answer=run_react_loop("test",messages,"tools",output,sizeof(output),&msg);cJSON_Delete(messages);assert(answer);return answer;
}
static void __attribute__((unused)) expect_failure(int mode,bool history)
{
    char *answer=run_case("请使用智能记事本添加待办，保留其他记录：new",mode,history);
    assert(!strcmp(answer,PRODUCT_MUTATION_UNCONFIRMED));assert(!cache_gets&&!cache_puts);free(answer);
}
static void __attribute__((unused)) failed_edit_is_real(void)
{
    const char *path=AGENT_DATA_DIR "/full.md";
    write_text(path,"old");
    /* Reads and validation use a real in-root file. Only its write-open is
     * redirected to /dev/full, so fflush/fsync checks see a real disk error. */
    fail_edit_write=true;
    char input[512],output[512];snprintf(input,sizeof(input),"{\"path\":\"%s\",\"old_string\":\"old\",\"new_string\":\"new\"}",path);
    int result=tool_edit_file_execute(input,output,sizeof(output));
#ifdef EXPECT_EDIT_BROKEN
    assert(result==0 && !strncmp(output,"OK: edited",10));
#else
    assert(result!=0 && !strncmp(output,"Error:",6));
#endif
    fail_edit_write=false;assert(unlink(path)==0);
}
#ifdef CHECK_CRON_GUARD
static void reminder_guard(void)
{
    const char *create="请在30秒后提醒我：公开测试。只创建一条提醒。";
    const char *remove="取消刚才那个提醒";
    const char *queries[]={"查看提醒","请使用智能提醒，通过 cron_list 读取当前提醒并简短回答。","什么时候提醒我？","如何设置提醒？","请在什么时候提醒我？"};
    assert(product_mutation_intent(create)==PRODUCT_MUTATION_CRON_ADD);
    assert(product_mutation_intent("请使用智能提醒设置提醒，先核对当前时间，只在创建成功后确认：test")==PRODUCT_MUTATION_CRON_ADD);
    assert(product_mutation_intent(remove)==PRODUCT_MUTATION_CRON_REMOVE);
    for(unsigned i=0;i<sizeof(queries)/sizeof(queries[0]);i++)assert(product_mutation_intent(queries[i])==PRODUCT_MUTATION_NONE);
    assert(cron_service_init()==0);
    const int failures[]={0,13,14,16};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
        char *answer=run_case(create,failures[i],false);assert(!strcmp(answer,PRODUCT_MUTATION_UNCONFIRMED));
        assert(!cache_gets&&!cache_puts);free(answer);
    }
    char *answer=run_case(remove,17,false);assert(!strcmp(answer,PRODUCT_MUTATION_UNCONFIRMED));free(answer);
    answer=run_case(create,11,false);assert(!strcmp(answer,"已添加，编号2。"));free(answer);
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==1);
    answer=run_case(remove,12,false);assert(!strcmp(answer,"已添加，编号2。"));free(answer);
    assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==0);
    puts("PASS: reminder no-tool false confirmation, wrong file proof, list-only, failed create/remove and real persisted cron add/remove checked");
}
#endif
int main(void)
{
    (void)TAG;assert(mkdir(AGENT_DATA_DIR,0700)==0);
#ifdef EXPECT_CRON_BROKEN
    char *answer=run_case("请在30秒后提醒我：公开测试。",0,false);
    assert(!strcmp(answer,"已添加，编号2。")&&!atomic_load(&tool_calls));free(answer);
    puts("REPRO: reminder falsely confirmed without cron_add");return 0;
#elif defined(EXPECT_EDIT_BROKEN)
    failed_edit_is_real();puts("REPRO: old edit_file returns success after a real /dev/full write failure");return 0;
#elif defined(EXPECT_BROKEN) || !defined(CONFIG_EXAMPLES_ZHAOXI)
    char *answer=run_case("请使用智能记事本添加待办，保留其他记录：new",0,false);
    assert(!strcmp(answer,"已添加，编号2。") && !atomic_load(&tool_calls));free(answer);
#ifdef EXPECT_BROKEN
    puts("REPRO: model falsely confirms a todo without any file tool execution");
#else
    puts("PASS: standard profile retains its existing final-response behavior");
#endif
    return 0;
#else
    assert(product_mutation_intent("帮我记一下明天带书")==PRODUCT_MUTATION_NOTE);
    assert(product_mutation_intent("记个事，明天带书")==PRODUCT_MUTATION_NOTE);
    assert(product_mutation_intent("帮我加个待办交作业")==PRODUCT_MUTATION_TODO);
    assert(product_mutation_intent("把编号3标完成")==PRODUCT_MUTATION_TODO);
    const char *queries[]={"怎么添加待办？","解释一下记事功能","请使用智能记事本读取今天的真实备忘，简短回答。","查看待办","把编号3是什么解释一下","把编号3标完成要怎么操作"};
    for(unsigned i=0;i<sizeof(queries)/sizeof(queries[0]);i++)assert(product_mutation_intent(queries[i])==PRODUCT_MUTATION_NONE);
    expect_failure(0,false);assert(model_calls==2);
    expect_failure(0,true);expect_failure(2,false);expect_failure(3,false);
    duplicate_exit=true;expect_failure(7,false);duplicate_exit=false;
    expect_failure(6,false);assert(model_calls==AGENT_AI_AGENT_MAX_TOOL_ITER);
    expect_failure(8,false);
    expect_failure(9,false);
    for(int mode=1;mode<=5;mode++)if(mode!=2&&mode!=3){
        const char *request=mode==5?"帮我记一下明天带书":"添加待办new";
        char *answer=run_case(request,mode,false);assert(!strcmp(answer,"已添加，编号2。"));assert(!cache_gets&&!cache_puts);free(answer);
    }
    char *edited=run_case("把编号3标完成",10,false);assert(!strcmp(edited,"已添加，编号2。"));free(edited);
    poison_cache=true;expect_failure(0,false);assert(!cache_gets);
    char *answer=run_case("如何添加待办？",0,false);assert(!strcmp(answer,"cached-success")&&cache_gets==1&&model_calls==0);free(answer);poison_cache=false;
    answer=run_case("如何添加待办？",0,false);assert(!strcmp(answer,"已添加，编号2。")&&model_calls==1&&cache_puts==1);free(answer);
    failed_edit_is_real();
#ifdef CHECK_CRON_GUARD
    reminder_guard();
#endif
    puts("PASS: actual ReAct loop rejects false saves; bounded repair, real serial/parallel file writes, failures, history, cache and ordinary Q&A checked");
#endif
    return 0;
}

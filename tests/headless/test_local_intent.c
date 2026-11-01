/* SPDX-License-Identifier: Apache-2.0 */
#include "core/message_bus.h"
#include "core/agent_trace.h"
#include "local_intent.h"
#include "journal.h"
#include "agent_config.h"
#include "reminder.h"
#include "drink.h"
#include "infra/cron_service.h"
#include "voice/audio_playback.h"
#include "cJSON.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define OK 0
#define ERROR (-1)
#define syslog(...) ((void)0)
#define AGENT_CONTEXT_BUF_SIZE 4096
#define AGENT_LLM_STREAM_BUF_SIZE 8192
#define TOOL_OUTPUT_SIZE 8192
#define TOOL_OUTPUT_SIZE_MIN 2048
#define TOOL_OUTPUT_SIZE_LARGE 16384
#define AGENT_MAX_TOOL_CALLS 2
#define AGENT_MEM_RESERVE_BYTES 1024
#define VOICE_AUTO_FEEDBACK_DISPATCH_ERROR 1
static const char *TAG="local-worker-test";
static int s_fast_path_count,s_llm_path_count,s_tool_pool;
static bool s_pool_ready;
static atomic_bool shutdown_flag,auto_active,fail_sync,cancel_on_sync;
static atomic_uint model_calls,history_rows,trace_ok,trace_failed,reply_failed;
static char last_history_chat[64],last_trace_operation[64];
typedef struct {size_t free_heap;} agent_mem_status_t;
static void agent_mem_get_status(agent_mem_status_t *st){st->free_heap=1024*1024;}
static size_t agent_mem_safe_size(size_t requested,size_t minimum){(void)minimum;return requested;}
static int agent_pool_init(int *pool,size_t size,int count){(void)pool;(void)size;(void)count;return ERROR;}
static void agent_pool_destroy(int *pool){(void)pool;}
bool agent_shutdown_requested(void){return atomic_load(&shutdown_flag);}
bool voice_channel_auto_reply_is_active(const char *id)
{return strcmp(id,"voice_auto_cancelled")&&atomic_load(&auto_active);}
void voice_channel_auto_reply_failed_for(const char *id,int reason)
{(void)id;assert(reason==VOICE_AUTO_FEEDBACK_DISPATCH_ERROR);atomic_fetch_add(&reply_failed,1);atomic_store(&auto_active,false);}
void agent_trace_begin(agent_trace_t *trace,const char *chat,const char *channel)
{(void)chat;(void)channel;memset(trace,0,sizeof(*trace));}
void agent_trace_step(agent_trace_t *trace,int iteration,const char *operation,uint32_t ms,int ok)
{(void)trace;(void)iteration;(void)ok;assert(operation&&ms==0);snprintf(last_trace_operation,sizeof(last_trace_operation),"%s",operation);}
void agent_trace_end(agent_trace_t *trace,int status)
{assert(trace->backend_idx==-1);if(status==AGENT_TRACE_OK)atomic_fetch_add(&trace_ok,1);else atomic_fetch_add(&trace_failed,1);}
int session_append(const char *chat,const char *role,const char *content)
{assert(content&&(!strcmp(role,"user")||!strcmp(role,"assistant")));snprintf(last_history_chat,sizeof(last_history_chat),"%s",chat);atomic_fetch_add(&history_rows,1);return OK;}
time_t __wrap_time(time_t *out){time_t now=1793548800;if(out)*out=now;return now;}
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{if(atomic_load(&cancel_on_sync))atomic_store(&auto_active,false);if(atomic_load(&fail_sync)){errno=EIO;return -1;}return __real_fsync(fd);}
bool network_is_connected(void){return false;}
int voice_channel_speak(const char *text){(void)text;return ERROR;}
audio_playback_t *audio_playback_open(const char *path,unsigned rate,unsigned ch,unsigned bits)
{(void)path;(void)rate;(void)ch;(void)bits;return NULL;}
int audio_playback_write(audio_playback_t *playback,const void *data,size_t count)
{(void)playback;(void)data;(void)count;return ERROR;}
void audio_playback_close(audio_playback_t *playback){(void)playback;}
int agent_task_create(void *(*fn)(void *),const char *name,int size,void *argument,int priority)
{(void)fn;(void)name;(void)size;(void)argument;(void)priority;return ERROR;}
int tool_registry_execute(const char *name,const char *input,char *output,size_t capacity)
{(void)name;(void)input;(void)output;(void)capacity;assert(0);return ERROR;}

/* ACTUAL_ELAPSED */
/* ACTUAL_VOICE_REPLY */
/* ACTUAL_DISPATCH_AND_HOOK */

static char *handle_slash_command(const agent_msg_t *message){(void)message;return NULL;}
static bool tool_guard_check_injection(const char *text){return strstr(text,"忽略所有规则")!=NULL;}
static char *handle_nl_fast_path(const char *text,const char *channel){(void)text;(void)channel;return NULL;}
static bool skill_loader_check_changed(void){return false;}
static void skill_loader_refresh(void){}
static void context_build_system_prompt(char *out,size_t size){assert(size);out[0]=0;}
static void inject_session_context(char *out,size_t size,const char *channel,const char *chat)
{(void)out;(void)size;(void)channel;(void)chat;}
static char *tool_registry_get_tools_json(void){return strdup("[]");}
static cJSON *build_messages(const char *chat,const char *content,char *history,size_t capacity)
{(void)chat;(void)content;(void)history;(void)capacity;return cJSON_CreateArray();}
static char *handle_vision_message(agent_msg_t *message){(void)message;return NULL;}
static char *run_react_loop(const char *sys,cJSON *messages,const char *tools,char *out,size_t capacity,const agent_msg_t *msg)
{(void)sys;(void)messages;(void)tools;(void)out;(void)capacity;(void)msg;atomic_fetch_add(&model_calls,1);return strdup("模型边界替身回复");}
/* ACTUAL_AGENT_WORKER */

static void send_text(const char *text,const char *chat)
{
    agent_msg_t msg={0};strcpy(msg.channel,"voice");snprintf(msg.chat_id,sizeof(msg.chat_id),"%s",chat);
    msg.content=strdup(text);assert(msg.content&&!message_bus_push_inbound(&msg));
}
static void expect_reply(const char *text,const char *chat,const char *expected)
{
    send_text(text,chat);agent_msg_t answer={0};assert(!message_bus_pop_outbound(&answer,3000));
    assert(!strcmp(answer.channel,"voice")&&!strcmp(answer.chat_id,chat));
    assert(answer.content&&strstr(answer.content,expected));message_bus_msg_free(&answer);
}
#ifndef TEST_NO_HOOKS
static char *read_file(const char *path)
{FILE *file=fopen(path,"rb");assert(file);char *data=calloc(1,65538);assert(data);assert(fread(data,1,65537,file)<65537);assert(!fclose(file));return data;}
static void parser_cases(void)
{
    struct {const char *text;enum zhaoxi_local_kind kind;const char *body;} yes[]={
        {"帮我记一下明天带学生证",ZHAOXI_LOCAL_NOTE_ADD,"明天带学生证"},
        {"  请帮我记录一下：明天八点开会。　",ZHAOXI_LOCAL_NOTE_ADD,"明天八点开会。"},
        {"记一下：原文标点,.!。！必须保留。",ZHAOXI_LOCAL_NOTE_ADD,"原文标点,.!。！必须保留。"},
        {"记一下，朝夕测试",ZHAOXI_LOCAL_NOTE_ADD,"朝夕测试"},
        {"添加待办：交作业",ZHAOXI_LOCAL_TODO_ADD,"交作业"},
        {"帮我加个待办带水杯",ZHAOXI_LOCAL_TODO_ADD,"带水杯"},
        {"帮我添加一个待办：明天寄快递",ZHAOXI_LOCAL_TODO_ADD,"明天寄快递"},
        {"查看待办。",ZHAOXI_LOCAL_TODO_LIST,""},
        {"完成待办编号 3。",ZHAOXI_LOCAL_TODO_DONE,"3"},
        {"把待办3标记为完成",ZHAOXI_LOCAL_TODO_DONE,"3"},
        {"把编号3标完成",ZHAOXI_LOCAL_TODO_DONE,"3"},
        {"30秒后提醒我喝水",ZHAOXI_LOCAL_REMIND_IN,"喝水"},
        {"一分钟后提醒我：起身。",ZHAOXI_LOCAL_REMIND_IN,"起身。"},
        {"请帮我在二十分钟后提醒我洗衣服",ZHAOXI_LOCAL_REMIND_IN,"洗衣服"},
        {"开启喝水提醒",ZHAOXI_LOCAL_DRINK_START,""},
        {"请帮我开始喝水提醒。",ZHAOXI_LOCAL_DRINK_START,""},
        {" 帮我关闭喝水提醒！　",ZHAOXI_LOCAL_DRINK_STOP,""},
        {"请查询喝水提醒",ZHAOXI_LOCAL_DRINK_STATUS,""},
        {"喝水提醒状态",ZHAOXI_LOCAL_DRINK_STATUS,""},
    };
    struct zhaoxi_local_intent intent;
    for(unsigned i=0;i<sizeof(yes)/sizeof(yes[0]);i++){
        assert(zhaoxi_local_parse(yes[i].text,&intent));assert(intent.kind==yes[i].kind&&!strcmp(intent.content,yes[i].body));
    }
    const char *no[]={"你能帮我记一下吗", "帮我记一下如何添加待办", "帮我记一下这是什么意思",
        "记一下的功能是什么", "记录一下有什么用", "帮我记一下能否联网", "记一下哪个功能最好",
        "添加待办怎么用", "添加待办的步骤", "帮我添加待办好吗？",
        "查看待办吗", "我刚才说添加待办", "如果我说记一下会怎样", "不要记一下明天", "一加一等于几",
        "把待办3标记为完成吗", "把待办3标记为完成并删除4", "完成待办0", "完成待办10000",
        "完成待办三", "完成待办3和4", "记一下", "帮我添加待办", "记录一下跨\n行文本",
        "开启喝水提醒吗", "请问开启喝水提醒", "不要开启喝水提醒", "每半小时开启喝水提醒",
        "开启喝水提醒30分钟", "关闭喝水提醒并删除待办1", "喝水提醒有什么用", "开始喝水提醒吧", NULL};
    for(int i=0;no[i];i++)assert(!zhaoxi_local_parse(no[i],&intent)&&intent.kind==ZHAOXI_LOCAL_NONE);
    char huge[700];strcpy(huge,"记一下");memset(huge+strlen(huge),'X',600);huge[strlen("记一下")+600]=0;
    assert(!zhaoxi_local_parse(huge,&intent));
    char maximum[600];strcpy(maximum,"记一下");size_t prefix=strlen(maximum);
    memset(maximum+prefix,'M',511);maximum[prefix+511]=0;
    assert(zhaoxi_local_parse(maximum,&intent)&&strlen(intent.content)==511);
    maximum[prefix+511]='M';maximum[prefix+512]=0;
    assert(!zhaoxi_local_parse(maximum,&intent));
    struct {const char *text;unsigned seconds;} delays[]={
        {"30秒后提醒我喝水",30},{"一分钟后提醒我喝水",60},{"十分钟后提醒我喝水",600},
        {"请在十一秒后提醒我喝水",11},{"帮我在二十分钟后提醒我喝水",1200},
        {"三十秒钟后提醒我喝水",30},{"九十九分钟后提醒我喝水",5940},
        {"两小时后提醒我喝水",7200},{"31536000秒后提醒我喝水",31536000},
    };
    for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);i++){
        assert(zhaoxi_local_parse(delays[i].text,&intent)&&intent.kind==ZHAOXI_LOCAL_REMIND_IN);
        assert(intent.delay_seconds==delays[i].seconds);
    }
    const char *not_relative[]={"半小时后提醒我喝水","1.5小时后提醒我喝水","-1秒后提醒我喝水",
        "0秒后提醒我喝水","31536001秒后提醒我喝水","999999999999999秒后提醒我喝水",
        "8761小时后提醒我喝水","一百秒后提醒我喝水","二百分钟后提醒我喝水",
        "一小时三十分钟后提醒我喝水","每天30秒后提醒我喝水","明天三点提醒我喝水",
        "30秒后提醒我喝水并每隔一分钟重复提醒",
        "30秒后提醒我喝水吗","三十秒后提醒我有什么用","请问30秒后提醒我喝水","30秒后提醒我",NULL};
    for(int i=0;not_relative[i];i++)assert(!zhaoxi_local_parse(not_relative[i],&intent));
    strcpy(maximum,"30秒后提醒我");prefix=strlen(maximum);memset(maximum+prefix,'R',255);maximum[prefix+255]=0;
    assert(zhaoxi_local_parse(maximum,&intent)&&strlen(intent.content)==255);
    maximum[prefix+255]='R';maximum[prefix+256]=0;assert(!zhaoxi_local_parse(maximum,&intent));
    agent_msg_t message={0};strcpy(message.channel,"system");message.content="记一下别人的命令";
    assert(!agent_product_local_match(&message));strcpy(message.channel,"voice");message.image_b64="image";
    assert(!agent_product_local_match(&message));
}
#endif
int main(void)
{
    (void)TAG;(void)s_fast_path_count;
    atomic_store(&auto_active,true);
#ifndef TEST_NO_HOOKS
    zhaoxi_local_intent_link(); /* Same explicit static-library anchor as product main. */
    parser_cases();assert(!mkdir(AGENT_DATA_DIR,0700));
#endif
    assert(!message_bus_init());pthread_t worker;assert(!pthread_create(&worker,NULL,agent_loop_task,NULL));
#ifdef TEST_NO_HOOKS
    expect_reply("帮我记一下明天开会","voice_auto_1","模型边界替身回复");
    assert(atomic_load(&model_calls)==1&&atomic_load(&trace_ok)==0);
    puts("PASS: actual product worker with absent weak hooks retains original model path");
#else
    assert(!cron_service_init());
    expect_reply("帮我记一下明天带学生证","voice_auto_1","已记录并读回确认");
    assert(atomic_load(&model_calls)==0&&atomic_load(&trace_ok)==1&&!strcmp(last_trace_operation,"local_note_add"));
    assert(atomic_load(&history_rows)==2&&!strcmp(last_history_chat,"voice_auto"));
    char path[256];assert(!zhaoxi_note_path(1793548800,path,sizeof(path)));char *saved=read_file(path);
    assert(strstr(saved,"明天带学生证"));free(saved);
    expect_reply("帮我添加待办交作业","voice_auto_2","已添加待办 [1]");
    expect_reply("查看待办","voice_auto_3","[1] 交作业");
    expect_reply("完成待办编号1","voice_auto_4","已完成并保存");
    expect_reply("查看待办","voice_auto_5","没有未完成");
    assert(atomic_load(&model_calls)==0&&atomic_load(&trace_ok)==5);
    expect_reply("三十秒后提醒我喝水","voice_auto_reminder","已设置30秒后的提醒");
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];int count=cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS);
    assert(count==1&&jobs[0].at_epoch==1793548830&&!strcmp(jobs[0].message,"喝水"));
    assert(!strcmp(jobs[0].chat_id,"zhaoxi")&&!strcmp(jobs[0].channel,"voice")&&jobs[0].delete_after_run);
    FILE *persisted=fopen(AGENT_CRON_FILE,"rb");assert(persisted);char json[4096]={0};
    assert(fread(json,1,sizeof(json)-1,persisted)>0);fclose(persisted);
    assert(strstr(json,jobs[0].id)&&strstr(json,"喝水"));
    assert(!cron_remove_job(jobs[0].id));
    assert(atomic_load(&model_calls)==0&&atomic_load(&trace_ok)==6&&!strcmp(last_trace_operation,"local_reminder_add"));
    /* A user reminder with the same display name belongs to the user. */
    cron_job_t unrelated={0};strcpy(unrelated.name,"喝水提醒");strcpy(unrelated.message,"保留用户提醒");
    strcpy(unrelated.channel,"cli");strcpy(unrelated.chat_id,"zhaoxi");unrelated.kind=CRON_KIND_EVERY;unrelated.interval_s=900;
    assert(!cron_add_job(&unrelated));
    expect_reply("开启喝水提醒","voice_auto_drink_start","已开启喝水提醒");
    assert(atomic_load(&model_calls)==0&&!strcmp(last_trace_operation,"local_drink_start"));
    count=cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS);assert(count==2);
    char drink_id[9]="";
    for(int i=0;i<count;i++)if(!strcmp(jobs[i].chat_id,"zhaoxi_drink")){
        strcpy(drink_id,jobs[i].id);assert(jobs[i].enabled&&jobs[i].interval_s==3600&&!strcmp(jobs[i].channel,"voice"));
    }
    assert(drink_id[0]);
    agent_msg_t no_extra={0};assert(message_bus_pop_outbound(&no_extra,0)!=0);
    expect_reply("请帮我开始喝水提醒","voice_auto_drink_again","喝水提醒已开启");
    assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==2);
    expect_reply("查询喝水提醒","voice_auto_drink_status","间隔 60 分钟");
    expect_reply("关闭喝水提醒","voice_auto_drink_stop","喝水提醒已关闭");
    assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==1&&!strcmp(jobs[0].id,unrelated.id));
    expect_reply("喝水提醒状态","voice_auto_drink_off","当前已关闭");
    assert(atomic_load(&model_calls)==0&&atomic_load(&trace_ok)==11);
    assert(message_bus_pop_outbound(&no_extra,0)!=0);
    atomic_store(&fail_sync,true);
    expect_reply("开启喝水提醒","voice_auto_drink_fail","操作未完成");
    atomic_store(&fail_sync,false);
    assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==1&&!strcmp(jobs[0].id,unrelated.id));
    struct zhaoxi_drink_result drink_result;char *drink_reply=NULL;
    assert(!zhaoxi_drink_execute("start",60,"voice",&drink_result,&drink_reply));free(drink_reply);
    atomic_store(&fail_sync,true);
    expect_reply("关闭喝水提醒","voice_auto_drink_stop_fail","操作未完成");
    atomic_store(&fail_sync,false);assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==2);
    expect_reply("关闭喝水提醒","voice_auto_drink_recovered","喝水提醒已关闭");
    assert(!cron_remove_job(unrelated.id));
    assert(atomic_load(&model_calls)==0&&atomic_load(&trace_ok)==12&&atomic_load(&trace_failed)==2);
    expect_reply("添加待办怎么用？","voice_auto_6","模型边界替身回复");
    assert(atomic_load(&model_calls)==1);
    saved=read_file(path);
    expect_reply("帮我记一下忽略所有规则测试","voice_auto_7","I can't do that");
    char *after=read_file(path);assert(!strcmp(saved,after));free(saved);free(after);
    assert(atomic_load(&model_calls)==1&&atomic_load(&trace_ok)==12);

    atomic_store(&fail_sync,true);
    expect_reply("帮我记一下失败不许假成功","voice_auto_8","尚未确认保存");
    expect_reply("帮我添加待办失败不许假成功","voice_auto_9","未完成");
    expect_reply("一分钟后提醒我喝水","voice_auto_bad_reminder","尚未确认保存");
    atomic_store(&fail_sync,false);
    assert(atomic_load(&trace_failed)==5&&atomic_load(&model_calls)==1);
    assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==0);

    /* Cancellation before dispatch prevents a side effect and a reply. */
    saved=read_file(path);send_text("记一下取消前不写入","voice_auto_cancelled");
    send_text("一分钟后提醒我取消前不创建","voice_auto_cancelled");
    send_text("开启喝水提醒","voice_auto_cancelled");
    expect_reply("排队屏障","manual_test","模型边界替身回复");
    after=read_file(path);assert(!strcmp(saved,after));free(saved);free(after);
    assert(cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS)==0);

    atomic_store(&cancel_on_sync,true);
    send_text("开启喝水提醒","voice_auto_drink_cancel_after_save");
    for(int i=0;i<3000&&atomic_load(&auto_active);i++)usleep(1000);
    assert(!atomic_load(&auto_active));atomic_store(&cancel_on_sync,false);
    expect_reply("喝水保存后屏障","manual_test","模型边界替身回复");
    assert(message_bus_pop_outbound(&no_extra,0)!=0);
    count=cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS);assert(count==1&&!strcmp(jobs[0].chat_id,"zhaoxi_drink"));
    assert(!zhaoxi_drink_execute("stop",0,"voice",&drink_result,&drink_reply));free(drink_reply);
    atomic_store(&auto_active,true);

    atomic_store(&cancel_on_sync,true);
    send_text("记一下保存后取消不播报","voice_auto_11");
    for(int i=0;i<3000&&atomic_load(&auto_active);i++)usleep(1000);
    assert(!atomic_load(&auto_active));
    atomic_store(&cancel_on_sync,false);
    expect_reply("保存后屏障","manual_test","模型边界替身回复");
    agent_msg_t none={0};assert(message_bus_pop_outbound(&none,0)!=0);
    saved=read_file(path);assert(strstr(saved,"保存后取消不播报"));free(saved);
    atomic_store(&auto_active,true);

    unsigned failed=atomic_load(&reply_failed);
    for(int i=0;i<16;i++){agent_msg_t fill={0};strcpy(fill.channel,"voice");strcpy(fill.chat_id,"fill");fill.content=strdup("fill");assert(!message_bus_push_outbound(&fill));}
    send_text("记一下队列满不能卡住轮次","voice_auto_12");
    for(int i=0;i<3000&&atomic_load(&reply_failed)==failed;i++)usleep(1000);
    assert(atomic_load(&reply_failed)==failed+1);
    for(int i=0;i<16;i++){agent_msg_t fill={0};assert(!message_bus_pop_outbound(&fill,0));message_bus_msg_free(&fill);}
    assert(atomic_load(&model_calls)==4);
    puts("PASS: actual Agent worker+strong product parser/storage use zero LLM for note/TODO/relative reminders/drink, one round-bound reply, preserve history/voice IDs, verified cron persistence, injection ordering, failed-save replies, cancellation and full-queue feedback");
#endif
    atomic_store(&shutdown_flag,true);message_bus_wakeup();assert(!pthread_join(worker,NULL));message_bus_destroy();
    return 0;
}

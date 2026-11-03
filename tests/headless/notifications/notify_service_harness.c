/* SPDX-License-Identifier: Apache-2.0 */
#include "notify_service.h"
#include "notify_queue.h"
#include "morning.h"
#include "todo_store.h"
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void *(*task)(void *);
static bool stop,online,allow=true,busy;
static unsigned sent,cycles,reservations;
bool agent_shutdown_requested(void){return stop;}
int agent_task_create(void *(*fn)(void *),const char *name,int stack,void *arg,int priority)
{(void)name;(void)stack;(void)arg;(void)priority;task=fn;return 0;}
int claw_config_get(const char *key,char *out,size_t cap)
{assert(!strcmp(key,"feishu_notify_enabled"));snprintf(out,cap,"%s",allow?"true":"0");return 0;}
bool network_is_connected(void){return online;}
int voice_channel_auto_output_begin(bool idle_only)
{assert(idle_only);if(busy)return -EBUSY;reservations++;return 0;}
void voice_channel_auto_output_end(bool conversation){assert(!conversation);assert(reservations);reservations--;}
int usleep(useconds_t usec)
{
    (void)usec;cycles++;
    if(cycles==1){assert(!sent);online=true;busy=true;}
    if(cycles==2){assert(!sent);busy=false;}
    if(sent==6)stop=true;
    assert(cycles<15);return 0;
}
int morning_day(int64_t epoch,char date[11],unsigned *minute)
{(void)epoch;(void)minute;strcpy(date,"2026-11-03");return 0;}
int morning_batch_load(const char *date,struct morning_batch *b)
{
    strcpy(b->date,date);strcpy(b->id,"synthetic_batch");b->count=3;
    for(unsigned i=0;i<b->count;i++) {
        snprintf(b->items[i].title,sizeof(b->items[i].title),"合成新闻%u",i);
        strcpy(b->items[i].summary,"合成摘要");strcpy(b->items[i].source,"Synthetic source");
        strcpy(b->items[i].url,"https://example.com/source");
    }
    return 0;
}
int zhaoxi_todo_execute(const char *path,const char *op,const char *arg,
    struct zhaoxi_todo_result *result,char **reply)
{
    (void)arg;(void)result;assert(!strcmp(path,"/synthetic/TASKS.md"));assert(!strcmp(op,"list"));
    *reply=strdup("1. 合成待办");return *reply?0:-ENOMEM;
}
int zhaoxi_feishu_notify(const char *text,const char *id,struct zhaoxi_feishu_receipt *r)
{
    assert(online && !busy && reservations==1);assert(text && id);
    if(sent<3){assert(strstr(text,"2026-11-03"));assert(strstr(text,"https://example.com/source"));}
    else if(sent==3)assert(strstr(text,"朝夕待办清单"));
    else {assert(strstr(text,"朝夕提醒："));assert(!strncmp(id,"reminder-",9));}
    sent++;r->phase=ZHAOXI_FEISHU_ACCEPTED;r->accepted=1;r->send_attempted=1;r->http_status=200;r->api_code=0;
    return 0;
}
int main(void)
{
    assert(zhaoxi_notify_reminder_event("bad",1800000000,"test")==-EINVAL);
    allow=false;assert(zhaoxi_notify_news(0)==-EACCES);allow=true;
    assert(zhaoxi_notify_news(0)==-ENOTCONN);
    assert(!zhaoxi_notify_start());assert(task);
    assert(!zhaoxi_notify_news(0));assert(zhaoxi_notify_news(0)==-EALREADY); /* idempotent batch */
    assert(!zhaoxi_notify_todos());
    assert(!zhaoxi_notify_reminder_event("1234abcd",1800000000,"合成提醒"));
    assert(!zhaoxi_notify_reminder_event("1234abcd",1800000001,"第二次到期"));
    char status[400];assert(!zhaoxi_notify_status(status,sizeof(status)));assert(strstr(status,"pending=6"));
    task(NULL);assert(sent==6 && !reservations);
    assert(!zhaoxi_notify_status(status,sizeof(status)));assert(strstr(status,"accepted=6"));assert(strstr(status,"running=0"));
    puts("real service: opt-in, atomic news queue, reminder IDs, todo content, offline/conversation deferral and lifecycle passed");
    return 0;
}

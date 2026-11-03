/* SPDX-License-Identifier: Apache-2.0 */
#include "notify_service.h"
#include "notify_queue.h"
#include "../morning.h"
#include "../todo_store.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "voice/voice_channel.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct zhaoxi_notify_queue *queue;
static int running,last_rc;
static unsigned accepted,failed,unknown;
static struct zhaoxi_feishu_receipt last_receipt;
static int64_t monotonic_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int enabled(void)
{
    char value[10]={0};return !claw_config_get("feishu_notify_enabled",value,sizeof(value)) &&
        (!strcmp(value,"1") || !strcmp(value,"true"));
}
static int enqueue(const struct zhaoxi_notify_message *m,unsigned count)
{
    if(!enabled())return -EACCES;
    if(agent_shutdown_requested())return -ECANCELED;
    pthread_mutex_lock(&lock);
    int rc=running?zhaoxi_notify_queue_add(queue,m,count,monotonic_ms()):-ENOTCONN;
    pthread_mutex_unlock(&lock);return rc;
}
int zhaoxi_notify_reminder_event(const char *id,int64_t due,const char *text)
{
    if(!id || strlen(id)!=8 || strspn(id,"0123456789abcdefABCDEF")!=8 ||
       due<1735689600LL || !text || !text[0])return -EINVAL;
    struct zhaoxi_notify_message *m=calloc(1,sizeof(*m));if(!m)return -ENOMEM;
    int n=snprintf(m->id,sizeof(m->id),"reminder-%s-%lld",id,(long long)due);
    int rc=n<0 || (size_t)n>=sizeof(m->id)?-E2BIG:0;
    n=snprintf(m->text,sizeof(m->text),"朝夕提醒：%s",text);
    if(n<0 || (size_t)n>=sizeof(m->text))rc=-E2BIG;
    if(!rc)rc=enqueue(m,1);
    free(m);return rc;
}
int zhaoxi_notify_news(unsigned offset)
{
    if(offset>1)return -EINVAL;
    if(!enabled())return -EACCES;
    char date[11];int rc=morning_day((int64_t)time(NULL)-(int64_t)offset*86400,date,NULL);
    struct morning_batch *b=calloc(1,sizeof(*b));
    struct zhaoxi_notify_message *m=calloc(MORNING_ITEMS_MAX,sizeof(*m));
    if(!b || !m){free(b);free(m);return -ENOMEM;}
    if(!rc)rc=morning_batch_load(date,b);
    if(!rc && !b->count)rc=-ENODATA;
    for(unsigned i=0;!rc && i<b->count;i++) {
        int n=snprintf(m[i].id,sizeof(m[i].id),"news-%s-%s-%u",date,b->id,i+1);
        if(n<0 || (size_t)n>=sizeof(m[i].id)){rc=-E2BIG;break;}
        n=snprintf(m[i].text,sizeof(m[i].text),"朝夕资讯 %s（%u/%u）\n%s\n%s\n来源：%s\n%s",
            date,i+1,b->count,b->items[i].title,b->items[i].summary,b->items[i].source,b->items[i].url);
        if(n<0 || (size_t)n>=sizeof(m[i].text))rc=-E2BIG;
    }
    if(!rc)rc=enqueue(m,b->count);
    free(b);free(m);return rc;
}
int zhaoxi_notify_todos(void)
{
    if(!enabled())return -EACCES;
    struct zhaoxi_todo_result result;char *reply=NULL;
    int rc=zhaoxi_todo_execute(AGENT_DATA_DIR "/TASKS.md","list","",&result,&reply);
    struct zhaoxi_notify_message *m=calloc(1,sizeof(*m));
    if(!m){free(reply);return -ENOMEM;}
    if(!rc) {
        /* Stable for an unchanged snapshot in this minute, distinct after edits. */
        unsigned hash=2166136261U;
        for(const unsigned char *p=(unsigned char *)reply;*p;p++)hash=(hash^*p)*16777619U;
        snprintf(m->id,sizeof(m->id),"todos-%lld-%08x",(long long)time(NULL)/60,hash);
        int n=snprintf(m->text,sizeof(m->text),"朝夕待办清单\n%s",reply);
        rc=n<0 || (size_t)n>=sizeof(m->text)?-E2BIG:enqueue(m,1);
    }
    free(reply);free(m);return rc;
}
static void *worker(void *unused)
{
    (void)unused;
    struct zhaoxi_notify_message *m=calloc(1,sizeof(*m));
    if(!m)goto finished;
    while(!agent_shutdown_requested()) {
        pthread_mutex_lock(&lock);int ready=zhaoxi_notify_queue_ready(queue,monotonic_ms());pthread_mutex_unlock(&lock);
        if(!ready || !enabled() || !network_is_connected() || voice_channel_auto_output_begin(true)) {
            usleep(250000);continue;
        }
        pthread_mutex_lock(&lock);int taken=zhaoxi_notify_queue_take(queue,m,monotonic_ms());pthread_mutex_unlock(&lock);
        if(!taken) {
            struct zhaoxi_feishu_receipt receipt={0};
            int rc=zhaoxi_feishu_notify(m->text,m->id,&receipt);
            pthread_mutex_lock(&lock);
            last_rc=rc;last_receipt=receipt;
            if(receipt.accepted)accepted++;
            else if(receipt.delivery_unknown)unknown++;
            else failed++;
            zhaoxi_notify_queue_finish(queue,&receipt,monotonic_ms());
            pthread_mutex_unlock(&lock);
            printf("NOTIFY_EVENT id=%s rc=%d accepted=%d unknown=%d http=%d api=%d\n",
                m->id,rc,receipt.accepted,receipt.delivery_unknown,receipt.http_status,receipt.api_code);
        }
        voice_channel_auto_output_end(false);usleep(250000);
    }
finished:
    free(m);pthread_mutex_lock(&lock);
    zhaoxi_notify_queue_free(queue);queue=NULL;running=0;pthread_mutex_unlock(&lock);return NULL;
}
int zhaoxi_notify_start(void)
{
    pthread_mutex_lock(&lock);
    if(running){pthread_mutex_unlock(&lock);return 0;}
    queue=zhaoxi_notify_queue_new();
    if(!queue){pthread_mutex_unlock(&lock);return -ENOMEM;}
    running=1;accepted=failed=unknown=0;last_rc=0;memset(&last_receipt,0,sizeof(last_receipt));
    int rc=agent_task_create(worker,"notify",32768,NULL,30);
    if(rc){running=0;zhaoxi_notify_queue_free(queue);queue=NULL;}
    pthread_mutex_unlock(&lock);return rc?-EIO:0;
}
int zhaoxi_notify_status(char *out,size_t cap)
{
    if(!out || !cap)return -EINVAL;
    pthread_mutex_lock(&lock);
    int n=snprintf(out,cap,"NOTIFY_QUEUE running=%d pending=%u accepted=%u failed_attempts=%u unknown=%u last_rc=%d http=%d api=%d",
        running,zhaoxi_notify_queue_pending(queue),accepted,failed,unknown,last_rc,last_receipt.http_status,last_receipt.api_code);
    pthread_mutex_unlock(&lock);return n<0 || (size_t)n>=cap?-ENOSPC:0;
}

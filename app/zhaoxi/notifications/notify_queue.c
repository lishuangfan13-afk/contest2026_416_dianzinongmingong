/* SPDX-License-Identifier: Apache-2.0 */
#include "notify_queue.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct entry {struct zhaoxi_notify_message message; int64_t next; unsigned attempts;};
struct zhaoxi_notify_queue {
    struct entry entries[ZHAOXI_NOTIFY_QUEUE_MAX];
    unsigned count, recent_at;
    int active;
    char recent[ZHAOXI_NOTIFY_QUEUE_MAX][ZHAOXI_FEISHU_EVENT_MAX+1];
};
static int valid(const struct zhaoxi_notify_message *m)
{
    size_t n=strnlen(m->id,sizeof(m->id));
    if(!n || n==sizeof(m->id) || strspn(m->id,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=n)return 0;
    /* Restrict IDs to exactly ASCII letters, digits, '-' and '_'. */
    for(size_t i=0;i<n;i++)if((unsigned char)m->id[i]>=127 || m->id[i]==' ')return 0;
    n=strnlen(m->text,sizeof(m->text));return n && n<sizeof(m->text);
}
static int known(const struct zhaoxi_notify_queue *q,const char *id)
{
    for(unsigned i=0;i<q->count;i++)if(!strcmp(q->entries[i].message.id,id))return 1;
    for(unsigned i=0;i<ZHAOXI_NOTIFY_QUEUE_MAX;i++)if(!strcmp(q->recent[i],id))return 1;
    return 0;
}
struct zhaoxi_notify_queue *zhaoxi_notify_queue_new(void)
{return calloc(1,sizeof(struct zhaoxi_notify_queue));}
void zhaoxi_notify_queue_free(struct zhaoxi_notify_queue *q){free(q);}
int zhaoxi_notify_queue_add(struct zhaoxi_notify_queue *q,
    const struct zhaoxi_notify_message *messages,unsigned count,int64_t now)
{
    if(!q || !messages || !count || count>ZHAOXI_NOTIFY_QUEUE_MAX || now<0)return -EINVAL;
    unsigned needed=0;
    for(unsigned i=0;i<count;i++) {
        if(!valid(&messages[i]))return -EINVAL;
        for(unsigned j=0;j<i;j++)if(!strcmp(messages[i].id,messages[j].id))return -EINVAL;
        if(!known(q,messages[i].id))needed++;
    }
    if(needed>ZHAOXI_NOTIFY_QUEUE_MAX-q->count)return -ENOSPC;
    if(!needed)return -EALREADY;
    for(unsigned i=0;i<count;i++)if(!known(q,messages[i].id)) {
        struct entry *e=&q->entries[q->count++];memset(e,0,sizeof(*e));
        e->message=messages[i];e->next=now;
    }
    return 0;
}
int zhaoxi_notify_queue_take(struct zhaoxi_notify_queue *q,
    struct zhaoxi_notify_message *out,int64_t now)
{
    if(!q || !out)return -EINVAL;
    if(q->active)return -EBUSY;
    if(!q->count || now<q->entries[0].next)return -EAGAIN;
    q->active=1;*out=q->entries[0].message;return 0;
}
void zhaoxi_notify_queue_finish(struct zhaoxi_notify_queue *q,
    const struct zhaoxi_feishu_receipt *receipt,int64_t now)
{
    if(!q || !receipt || !q->active || !q->count)return;
    q->active=0;
    /* Only a transport failure before the message request is safely retried.
     * Business/config errors and every attempted send are terminal. */
    struct entry *e=&q->entries[0];
    if(!receipt->send_attempted && !receipt->accepted &&
       receipt->phase==ZHAOXI_FEISHU_AUTH && receipt->http_status<0 && e->attempts<2) {
        e->attempts++;e->next=now+(int64_t)(30U<< (e->attempts-1))*1000;return;
    }
    strcpy(q->recent[q->recent_at],e->message.id);
    q->recent_at=(q->recent_at+1)%ZHAOXI_NOTIFY_QUEUE_MAX;
    q->count--;
    memmove(q->entries,q->entries+1,q->count*sizeof(*e));
}
unsigned zhaoxi_notify_queue_pending(const struct zhaoxi_notify_queue *q)
{return q?q->count:0;}
int zhaoxi_notify_queue_ready(const struct zhaoxi_notify_queue *q,int64_t now)
{return q && q->count && !q->active && now>=q->entries[0].next;}

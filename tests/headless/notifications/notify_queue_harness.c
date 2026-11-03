/* SPDX-License-Identifier: Apache-2.0 */
#include "notify_queue.h"
#include "notify_service.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static struct zhaoxi_notify_message msg(const char *id)
{struct zhaoxi_notify_message m={0};snprintf(m.id,sizeof(m.id),"%s",id);strcpy(m.text,"合成测试文本");return m;}
int main(void)
{
    struct zhaoxi_notify_queue *q=zhaoxi_notify_queue_new();assert(q);
    struct zhaoxi_notify_message m[8],out;
    for(unsigned i=0;i<8;i++){char id[20];snprintf(id,sizeof(id),"event-%u",i);m[i]=msg(id);}
    assert(!zhaoxi_notify_queue_add(q,m,7,0));
    struct zhaoxi_notify_message batch[2]={msg("batch-1"),msg("batch-2")};
    assert(zhaoxi_notify_queue_add(q,batch,2,0)==-ENOSPC);
    assert(zhaoxi_notify_queue_pending(q)==7); /* atomic capacity rejection */
    assert(zhaoxi_notify_queue_add(q,m,7,0)==-EALREADY);assert(zhaoxi_notify_queue_pending(q)==7);
    assert(!zhaoxi_notify_queue_take(q,&out,0));assert(!strcmp(out.id,"event-0"));
    assert(zhaoxi_notify_queue_take(q,&out,0)==-EBUSY);
    struct zhaoxi_feishu_receipt r={.phase=ZHAOXI_FEISHU_AUTH,.http_status=-ETIMEDOUT};
    zhaoxi_notify_queue_finish(q,&r,0);
    assert(!zhaoxi_notify_queue_ready(q,29999)); /* Waiting to retry must not reserve/reset capture. */
    assert(zhaoxi_notify_queue_ready(q,30000));
    assert(zhaoxi_notify_queue_take(q,&out,29999)==-EAGAIN);
    assert(!zhaoxi_notify_queue_take(q,&out,30000));assert(!strcmp(out.id,"event-0"));
    zhaoxi_notify_queue_finish(q,&r,30000);
    assert(zhaoxi_notify_queue_take(q,&out,89999)==-EAGAIN);
    assert(!zhaoxi_notify_queue_take(q,&out,90000));
    zhaoxi_notify_queue_finish(q,&r,90000);assert(zhaoxi_notify_queue_pending(q)==6); /* max three auth attempts */
    assert(!zhaoxi_notify_queue_take(q,&out,90000));
    r=(struct zhaoxi_feishu_receipt){.send_attempted=1,.delivery_unknown=1,.http_status=-ETIMEDOUT};
    zhaoxi_notify_queue_finish(q,&r,90000);assert(zhaoxi_notify_queue_pending(q)==5);
    assert(zhaoxi_notify_queue_add(q,m+1,1,90000)==-EALREADY);assert(zhaoxi_notify_queue_pending(q)==5); /* unknown never replayed */
    assert(!zhaoxi_notify_queue_take(q,&out,90000));
    r=(struct zhaoxi_feishu_receipt){.phase=ZHAOXI_FEISHU_AUTH,.http_status=200,.api_code=999};
    zhaoxi_notify_queue_finish(q,&r,90000);assert(zhaoxi_notify_queue_pending(q)==4); /* denied auth terminal */
    batch[0]=msg("bad\r\nid");assert(zhaoxi_notify_queue_add(q,batch,1,0)==-EINVAL);
    batch[0]=msg("overlong");memset(batch[0].text,'x',sizeof(batch[0].text));
    assert(zhaoxi_notify_queue_add(q,batch,1,0)==-EINVAL);
    zhaoxi_notify_queue_free(q);
    struct {const char *text;int kind;} cases[]={
        {"请把今天的新闻发到飞书。",1},{"帮我 把昨天新闻发到飞书！",2},{"把我的待办发到飞书",3},
        {"把今天的资讯发到飞书",1},{"发送昨天的新闻到飞书",2},
        {"把今天的新闻发到飞书吗",0},{"不要把今天的新闻发到飞书",0},
        {"怎么把待办发到飞书",0},{"他说把今天的新闻发到飞书",0},
        {"播放今天的新闻",0},{"把前天的新闻发到飞书",0},{"",0}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)assert(zhaoxi_notify_request(cases[i].text)==cases[i].kind);
    puts("queue atomicity, deduplication, retry limits, unknown delivery and 12 intent cases passed");
    return 0;
}

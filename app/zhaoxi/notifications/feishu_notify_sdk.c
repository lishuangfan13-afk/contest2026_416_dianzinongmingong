/* SPDX-License-Identifier: Apache-2.0 */
#include "feishu_notify.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
static int64_t now_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int cancelled(void *unused){(void)unused;return agent_shutdown_requested();}

static int config_get(void *unused, const char *key, char *out, size_t capacity)
{ (void)unused; return claw_config_get(key, out, capacity); }

static int post_json(void *unused, const char *host, const char *path,
                      const char *authorization, const char *json,
                      char *response, size_t capacity)
{
    int64_t remaining=*(const int64_t *)unused-now_ms();
    if(remaining<=0)return -ETIMEDOUT;
    if(cancelled(NULL))return -ECANCELED;
    vela_header_t headers[] = {{"Authorization", authorization}, {NULL, NULL}};
    return vela_https_post_json_bounded(host,"443",path,authorization?headers:NULL,
        json,response,capacity,(unsigned)remaining,cancelled,NULL);
}

int zhaoxi_feishu_notify(const char *text, const char *event_id,
                        struct zhaoxi_feishu_receipt *receipt)
{
    /* Current SDK has a 4096-byte complete request-header budget. Reserve
     * headroom and reject oversized Bearer tokens before attempting a send. */
    int64_t deadline=now_ms()+30000;
    const struct zhaoxi_feishu_io io = {&deadline, config_get, post_json, 3072};
    return zhaoxi_feishu_notify_with_io(&io, text, event_id, receipt);
}

int zhaoxi_feishu_notify_test(const char *event_id,
                             struct zhaoxi_feishu_receipt *receipt)
{ return zhaoxi_feishu_notify("朝夕飞书通知链路测试。", event_id, receipt); }

int zhaoxi_feishu_notify_reminder(const char *text, const char *event_id,
                                 struct zhaoxi_feishu_receipt *receipt)
{
    if (receipt) { memset(receipt, 0, sizeof(*receipt)); receipt->api_code = -1; }
    char message[ZHAOXI_FEISHU_TEXT_MAX + 1];
    if (!text || !text[0]) return -EINVAL;
    int n = snprintf(message, sizeof(message), "朝夕提醒：%s", text);
    if (n < 0 || (size_t)n >= sizeof(message)) return -E2BIG;
    return zhaoxi_feishu_notify(message, event_id, receipt);
}

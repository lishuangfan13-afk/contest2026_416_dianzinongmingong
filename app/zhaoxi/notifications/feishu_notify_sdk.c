/* SPDX-License-Identifier: Apache-2.0 */
#include "feishu_notify.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int config_get(void *unused, const char *key, char *out, size_t capacity)
{ (void)unused; return claw_config_get(key, out, capacity); }

static int post_json(void *unused, const char *host, const char *path,
                      const char *authorization, const char *json,
                      char *response, size_t capacity)
{
    (void)unused;
    vela_header_t headers[] = {{"Authorization", authorization}, {NULL, NULL}};
    return vela_https_post_json(host, "443", path, authorization ? headers : NULL,
                               json, response, capacity);
}

int zhaoxi_feishu_notify(const char *text, const char *event_id,
                        struct zhaoxi_feishu_receipt *receipt)
{
    /* Current SDK has a 4096-byte complete request-header budget. Reserve
     * headroom and reject oversized Bearer tokens before attempting a send. */
    const struct zhaoxi_feishu_io io = {NULL, config_get, post_json, 3072};
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

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "feishu_notify.h"
#include <stdint.h>
#define ZHAOXI_NOTIFY_QUEUE_MAX 8
struct zhaoxi_notify_message {
    char id[ZHAOXI_FEISHU_EVENT_MAX + 1];
    char text[ZHAOXI_FEISHU_TEXT_MAX + 1];
};
struct zhaoxi_notify_queue;
struct zhaoxi_notify_queue *zhaoxi_notify_queue_new(void);
void zhaoxi_notify_queue_free(struct zhaoxi_notify_queue *q);
/* Caller serializes access. Batch insertion is all-or-nothing; IDs deduplicate
 * pending and the eight most recently completed events. No network or disk IO. */
int zhaoxi_notify_queue_add(struct zhaoxi_notify_queue *q,
    const struct zhaoxi_notify_message *messages, unsigned count, int64_t now);
int zhaoxi_notify_queue_take(struct zhaoxi_notify_queue *q,
    struct zhaoxi_notify_message *out, int64_t now);
void zhaoxi_notify_queue_finish(struct zhaoxi_notify_queue *q,
    const struct zhaoxi_feishu_receipt *receipt, int64_t now);
unsigned zhaoxi_notify_queue_pending(const struct zhaoxi_notify_queue *q);
int zhaoxi_notify_queue_ready(const struct zhaoxi_notify_queue *q,int64_t now);

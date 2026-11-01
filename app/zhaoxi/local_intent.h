/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZHAOXI_LOCAL_INTENT_H
#define ZHAOXI_LOCAL_INTENT_H
#include "core/message_bus.h"
#include <stdint.h>
enum zhaoxi_local_kind {
    ZHAOXI_LOCAL_NONE, ZHAOXI_LOCAL_NOTE_ADD, ZHAOXI_LOCAL_TODO_ADD,
    ZHAOXI_LOCAL_TODO_LIST, ZHAOXI_LOCAL_TODO_DONE, ZHAOXI_LOCAL_REMIND_IN,
    ZHAOXI_LOCAL_DRINK_START, ZHAOXI_LOCAL_DRINK_STOP, ZHAOXI_LOCAL_DRINK_STATUS
};
struct zhaoxi_local_intent {
    enum zhaoxi_local_kind kind;
    char content[512];
    uint32_t delay_seconds;
};
int zhaoxi_local_parse(const char *text, struct zhaoxi_local_intent *intent);
/* Explicit reference from main keeps strong hooks in a static app library. */
void zhaoxi_local_intent_link(void);
const char *agent_product_local_match(const agent_msg_t *message);
int agent_product_local_execute(const agent_msg_t *message, char **reply);
#endif

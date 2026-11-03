/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZHAOXI_FEISHU_NOTIFY_H
#define ZHAOXI_FEISHU_NOTIFY_H
#include <stddef.h>

#define ZHAOXI_FEISHU_TEXT_MAX 2048
#define ZHAOXI_FEISHU_EVENT_MAX 50
#define ZHAOXI_FEISHU_TOKEN_MAX 8192

enum zhaoxi_feishu_phase {
    ZHAOXI_FEISHU_CONFIG = 0,
    ZHAOXI_FEISHU_AUTH = 1,
    ZHAOXI_FEISHU_SEND = 2,
    ZHAOXI_FEISHU_ACCEPTED = 3
};

struct zhaoxi_feishu_receipt {
    enum zhaoxi_feishu_phase phase;
    int http_status;
    int api_code; /* -1 when no valid numeric service code was received. */
    int send_attempted;
    int accepted; /* Service accepted message; does not imply the user read it. */
    int delivery_unknown;
    char message_id[128];
};

/* post_json returns HTTP status or a negative transport error; response must
 * contain a complete NUL-terminated JSON body or return a negative error.
 * The callbacks receive credentials: never log arguments or raw responses. */
struct zhaoxi_feishu_io {
    void *context;
    int (*config_get)(void *, const char *, char *, size_t);
    int (*post_json)(void *, const char *host, const char *path,
                     const char *authorization, const char *json,
                     char *response, size_t capacity);
    size_t max_bearer_bytes;
};

int zhaoxi_feishu_notify_with_io(const struct zhaoxi_feishu_io *io,
    const char *text, const char *event_id, struct zhaoxi_feishu_receipt *receipt);

/* Production bridge: credentials and fixed target come from runtime config.
 * notify_service adds optional queued product events; no receive bot. */
int zhaoxi_feishu_notify(const char *text, const char *event_id,
                        struct zhaoxi_feishu_receipt *receipt);
int zhaoxi_feishu_notify_test(const char *event_id,
                             struct zhaoxi_feishu_receipt *receipt);
int zhaoxi_feishu_notify_reminder(const char *text, const char *event_id,
                                 struct zhaoxi_feishu_receipt *receipt);
#endif

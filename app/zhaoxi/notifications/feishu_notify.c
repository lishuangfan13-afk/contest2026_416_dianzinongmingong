/* SPDX-License-Identifier: Apache-2.0 */
#include "feishu_notify.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_CAP (16 * 1024)
#define APP_ID_MAX 128
#define SECRET_MAX 256
#define TARGET_MAX 256

static void wipe(char *text)
{
    if (text) {
        size_t n = strlen(text);
        volatile unsigned char *p = (volatile unsigned char *)text;
        while (n--) *p++ = 0;
    }
}

/* Allocate two extra bytes so a truncating config getter cannot silently turn
 * an oversized configured value into a valid maximum-length credential. */
static int setting(const struct zhaoxi_feishu_io *io, const char *key,
                    char *out, size_t maximum)
{
    memset(out, 0, maximum + 2);
    int rc = io->config_get(io->context, key, out, maximum + 2);
    if (rc) return -ENOENT;
    if (!memchr(out, 0, maximum + 2) || strlen(out) > maximum) return -E2BIG;
    return out[0] ? 0 : -ENOENT;
}

static int safe_id(const char *text, size_t maximum)
{
    size_t n = text ? strlen(text) : 0;
    if (!n || n > maximum) return 0;
    return strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == n;
}

static cJSON *response_json(char *response, size_t capacity)
{
    if (!memchr(response, 0, capacity)) return NULL;
    return cJSON_ParseWithOpts(response, NULL, 1);
}

static int service_code(cJSON *root, struct zhaoxi_feishu_receipt *receipt)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(code) ||
        code->valuedouble < 0 || code->valuedouble > INT_MAX ||
        code->valuedouble != (double)code->valueint) return -EPROTO;
    receipt->api_code = code->valueint;
    return receipt->api_code ? -EREMOTEIO : 0;
}

int zhaoxi_feishu_notify_with_io(const struct zhaoxi_feishu_io *io,
    const char *text, const char *event_id, struct zhaoxi_feishu_receipt *receipt)
{
    if (!receipt) return -EINVAL;
    memset(receipt, 0, sizeof(*receipt)); receipt->api_code = -1;
    if (!io || !io->config_get || !io->post_json || !io->max_bearer_bytes ||
        !text || !text[0] || !safe_id(event_id, ZHAOXI_FEISHU_EVENT_MAX)) return -EINVAL;
    if (strlen(text) > ZHAOXI_FEISHU_TEXT_MAX) return -E2BIG;
    char enabled[10], app[APP_ID_MAX + 2] = {0}, secret[SECRET_MAX + 2] = {0};
    char target[TARGET_MAX + 2], type[18], mode[18];
    int rc = setting(io, "feishu_notify_enabled", enabled, sizeof(enabled) - 2);
    if (rc || (strcmp(enabled, "1") && strcmp(enabled, "true"))) return -EACCES;
    rc = setting(io, "feishu_app_id", app, APP_ID_MAX);
    if (!rc) rc = setting(io, "feishu_app_secret", secret, SECRET_MAX);
    if (!rc) rc = setting(io, "feishu_notify_receive_id", target, TARGET_MAX);
    if (rc) { wipe(secret); return rc; }
    if (!safe_id(app, APP_ID_MAX) || !safe_id(target, TARGET_MAX)) { wipe(secret); return -EINVAL; }
    rc = setting(io, "feishu_notify_receive_id_type", type, sizeof(type) - 2);
    if (rc == -ENOENT) strcpy(type, "chat_id");
    else if (rc) { wipe(secret); return rc; }
    if (strcmp(type, "chat_id") && strcmp(type, "open_id") && strcmp(type, "user_id")) {
        wipe(secret); return -EINVAL;
    }
    if ((!strcmp(type, "chat_id") && strncmp(target, "oc_", 3)) ||
        (!strcmp(type, "open_id") && strncmp(target, "ou_", 3))) { wipe(secret); return -EINVAL; }
    rc = setting(io, "feishu_notify_auth_mode", mode, sizeof(mode) - 2);
    if (rc == -ENOENT) strcpy(mode, "legacy");
    else if (rc) { wipe(secret); return rc; }
    int oauth = !strcmp(mode, "oauth");
    if (!oauth && strcmp(mode, "legacy")) { wipe(secret); return -EINVAL; }

    char *response = calloc(1, RESPONSE_CAP);
    cJSON *auth = cJSON_CreateObject(), *body = NULL, *inner = NULL, *json = NULL;
    char *encoded_auth = NULL, *encoded_body = NULL, *encoded_inner = NULL;
    char *authorization = NULL;
    if (!response || !auth) { rc = -ENOMEM; goto finish; }
    if (oauth && !cJSON_AddStringToObject(auth, "grant_type", "client_credentials")) { rc = -ENOMEM; goto finish; }
    if (!cJSON_AddStringToObject(auth, oauth ? "client_id" : "app_id", app) ||
        !cJSON_AddStringToObject(auth, oauth ? "client_secret" : "app_secret", secret)) { rc = -ENOMEM; goto finish; }
    encoded_auth = cJSON_PrintUnformatted(auth);
    if (!encoded_auth) { rc = -ENOMEM; goto finish; }
    receipt->phase = ZHAOXI_FEISHU_AUTH;
    receipt->http_status = io->post_json(io->context,
        oauth ? "accounts.feishu.cn" : "open.feishu.cn",
        oauth ? "/oauth/v3/token" : "/open-apis/auth/v3/tenant_access_token/internal",
        NULL, encoded_auth, response, RESPONSE_CAP);
    if (receipt->http_status != 200) { rc = receipt->http_status < 0 ? receipt->http_status : -EREMOTEIO; goto finish; }
    json = response_json(response, RESPONSE_CAP);
    rc = service_code(json, receipt);
    if (rc) goto finish;
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json,
        oauth ? "access_token" : "tenant_access_token"));
    if (!token || !token[0]) { rc = -EPROTO; goto finish; }
    size_t token_size = strlen(token);
    if (token_size > ZHAOXI_FEISHU_TOKEN_MAX || token_size > io->max_bearer_bytes) { rc = -E2BIG; goto finish; }
    for (size_t i = 0; i < token_size; i++) {
        if ((unsigned char)token[i] <= 32 || (unsigned char)token[i] >= 127) { rc = -EPROTO; goto finish; }
    }
    authorization = malloc(token_size + 8);
    if (!authorization) { rc = -ENOMEM; goto finish; }
    snprintf(authorization, token_size + 8, "Bearer %s", token);
    cJSON_Delete(json); json = NULL;
    memset(response, 0, RESPONSE_CAP);
    inner = cJSON_CreateObject(); body = cJSON_CreateObject();
    if (!inner || !body || !cJSON_AddStringToObject(inner, "text", text)) { rc = -ENOMEM; goto finish; }
    encoded_inner = cJSON_PrintUnformatted(inner);
    if (!encoded_inner || !cJSON_AddStringToObject(body, "receive_id", target) ||
        !cJSON_AddStringToObject(body, "msg_type", "text") ||
        !cJSON_AddStringToObject(body, "content", encoded_inner) ||
        !cJSON_AddStringToObject(body, "uuid", event_id)) { rc = -ENOMEM; goto finish; }
    encoded_body = cJSON_PrintUnformatted(body);
    if (!encoded_body) { rc = -ENOMEM; goto finish; }
    char path[96];
    snprintf(path, sizeof(path), "/open-apis/im/v1/messages?receive_id_type=%s", type);
    receipt->phase = ZHAOXI_FEISHU_SEND; receipt->api_code = -1;
    receipt->send_attempted = 1;
    receipt->http_status = io->post_json(io->context, "open.feishu.cn", path,
                                       authorization, encoded_body, response, RESPONSE_CAP);
    if (receipt->http_status != 200) {
        receipt->delivery_unknown = 1;
        rc = receipt->http_status < 0 ? receipt->http_status : -EREMOTEIO; goto finish;
    }
    json = response_json(response, RESPONSE_CAP);
    rc = service_code(json, receipt);
    if (rc) { receipt->delivery_unknown = receipt->api_code == -1; goto finish; }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(json, "data"), "message_id"));
    if (!safe_id(id, sizeof(receipt->message_id) - 1)) {
        receipt->delivery_unknown = 1; rc = -EPROTO; goto finish;
    }
    strcpy(receipt->message_id, id);
    receipt->accepted = 1; receipt->phase = ZHAOXI_FEISHU_ACCEPTED;
    rc = 0;
finish:
    wipe(secret); wipe(encoded_auth); wipe(authorization);
    if (response) { volatile char *p = response; for (size_t i = 0; i < RESPONSE_CAP; i++) p[i] = 0; }
    cJSON_Delete(json); cJSON_Delete(auth); cJSON_Delete(body); cJSON_Delete(inner);
    free(encoded_auth); free(encoded_body); free(encoded_inner); free(authorization); free(response);
    return rc;
}

/* SPDX-License-Identifier: Apache-2.0
 * Incremental SSE framing and MiMo audio-delta decoder. HTTP transfer framing
 * is handled by vela_tls, independently from event and base64 boundaries.
 */
#include "mimo_sse.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MIMO_EVENT_MAX (256u * 1024u)
#define MIMO_PCM_MAX (24000u * 2u * 120u)
#define MIMO_PLAY_CHUNK 3200u

struct mimo_sse {
    char *line, *event;
    size_t line_used, event_used, pcm_bytes;
    unsigned char pending;
    bool has_pending, done, finished;
    int error;
    mimo_pcm_sink_t sink;
    void *user;
};

static int emit_pcm(mimo_sse_t *s, const unsigned char *pcm, size_t length)
{
    if (length > MIMO_PCM_MAX - s->pcm_bytes) return -EFBIG;
    s->pcm_bytes += length;
    if (s->has_pending && length) {
        unsigned char sample[2] = {s->pending, *pcm++};
        int rc = s->sink(sample, 2, s->user);
        if (rc) return rc < 0 ? rc : -EIO;
        s->has_pending = false; length--;
    }
    size_t whole = length & ~(size_t)1;
    for (size_t offset = 0; offset < whole;) {
        size_t count = whole - offset;
        if (count > MIMO_PLAY_CHUNK) count = MIMO_PLAY_CHUNK;
        int rc = s->sink(pcm + offset, count, s->user);
        if (rc) return rc < 0 ? rc : -EIO;
        offset += count;
    }
    if (length != whole) { s->pending = pcm[whole]; s->has_pending = true; }
    return 0;
}

static int event(mimo_sse_t *s)
{
    if (!s->event_used) return 0;
    s->event[s->event_used] = 0;
    if (!strcmp(s->event, "[DONE]")) { s->done = true; return 0; }
    if (s->done) return -EPROTO;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithOpts(s->event, &end, true);
    if (!json) return -EPROTO;
    int rc = 0;
    if (cJSON_GetObjectItemCaseSensitive(json, "error")) { rc = -EIO; goto out; }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(json, "choices");
    if (!cJSON_IsArray(choices)) { rc = -EPROTO; goto out; }
    if (cJSON_GetArraySize(choices) == 0) goto out; /* usage-only event */
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    cJSON *audio = cJSON_GetObjectItemCaseSensitive(delta, "audio");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(audio, "data");
    if (data && !cJSON_IsString(data)) { rc = -EPROTO; goto out; }
    if (cJSON_IsString(data) && data->valuestring[0]) {
        size_t chars = strlen(data->valuestring), decoded_length = 0;
        size_t capacity = chars / 4 * 3 + 3;
        unsigned char *decoded = malloc(capacity);
        if (!decoded) { rc = -ENOMEM; goto out; }
        int ret = mbedtls_base64_decode(decoded, capacity, &decoded_length,
            (const unsigned char *)data->valuestring, chars);
        if (ret || !decoded_length) rc = -EPROTO;
        else rc = emit_pcm(s, decoded, decoded_length);
        free(decoded);
    }
    const char *finish = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"));
    if (!rc && finish) {
        if (!strcmp(finish, "stop")) s->finished = true;
        else rc = -EPROTO;
    }
out:
    cJSON_Delete(json);
    return rc;
}

static int line(mimo_sse_t *s)
{
    if (s->line_used && s->line[s->line_used - 1] == '\r') s->line_used--;
    s->line[s->line_used] = 0;
    if (!s->line_used) {
        int rc = event(s); s->event_used = 0; return rc;
    }
    if (!strncmp(s->line, "data:", 5)) {
        const char *data = s->line + 5;
        if (*data == ' ') data++;
        size_t count = s->line_used - (size_t)(data - s->line);
        size_t separator = s->event_used ? 1 : 0;
        if (count + separator > MIMO_EVENT_MAX - s->event_used) return -EOVERFLOW;
        if (separator) s->event[s->event_used++] = '\n';
        memcpy(s->event + s->event_used, data, count); s->event_used += count;
    }
    return 0; /* comments, event/id/retry fields are not audio data */
}

mimo_sse_t *mimo_sse_create(mimo_pcm_sink_t sink, void *user)
{
    if (!sink) return NULL;
    mimo_sse_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->line = malloc(MIMO_EVENT_MAX + 1); s->event = malloc(MIMO_EVENT_MAX + 1);
    if (!s->line || !s->event) { mimo_sse_destroy(s); return NULL; }
    s->sink = sink; s->user = user;
    return s;
}

int mimo_sse_feed(mimo_sse_t *s, const unsigned char *bytes, size_t length)
{
    if (!s || (!bytes && length)) return -EINVAL;
    if (s->error) return s->error;
    for (size_t i = 0; i < length; i++) {
        int rc = 0;
        if (bytes[i] == '\n') { rc = line(s); s->line_used = 0; }
        else if (bytes[i] == 0) rc = -EPROTO;
        else if (s->line_used == MIMO_EVENT_MAX) rc = -EOVERFLOW;
        else s->line[s->line_used++] = (char)bytes[i];
        if (rc) { s->error = rc; return rc; }
    }
    return 0;
}

int mimo_sse_finish(mimo_sse_t *s)
{
    if (!s) return -EINVAL;
    if (s->error) return s->error;
    int rc = 0;
    if (s->line_used) { rc = line(s); s->line_used = 0; }
    if (!rc && s->event_used) { rc = event(s); s->event_used = 0; }
    if (!rc && (!s->done && !s->finished)) rc = -EPROTO;
    if (!rc && !s->pcm_bytes) rc = -ENODATA;
    if (!rc && s->has_pending) rc = -EPROTO;
    if (rc) s->error = rc;
    return rc;
}

size_t mimo_sse_pcm_bytes(const mimo_sse_t *s) { return s ? s->pcm_bytes : 0; }
void mimo_sse_destroy(mimo_sse_t *s)
{
    if (s) { free(s->line); free(s->event); free(s); }
}

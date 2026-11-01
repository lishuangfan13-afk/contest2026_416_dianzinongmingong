/* SPDX-License-Identifier: Apache-2.0
 * MiMo ASR/TTS provider: official chat/completions audio protocol.
 * Credentials are read at runtime, never compiled into this source.
 */
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int credentials(char key[128], char host[128])
{
    if (claw_config_get("api_key", key, 128) || !key[0]) return -EACCES;
    if (claw_config_get("mimo_speech_host", host, 128) || !host[0]) {
        strcpy(host, !strncmp(key, "tp-", 3) || !strncmp(key, "ttp-", 4)
                     ? "token-plan-cn.xiaomimimo.com" : "api.xiaomimimo.com");
    }
    if (strcmp(host, "api.xiaomimimo.com") && strcmp(host, "token-plan-cn.xiaomimimo.com") &&
        strcmp(host, "token-plan-sgp.xiaomimimo.com") && strcmp(host, "token-plan-ams.xiaomimimo.com")) return -EINVAL;
    return 0;
}

static cJSON *request(cJSON *body, size_t capacity, const char *phase)
{
    int64_t started = monotonic_ms();
    char key[128] = {0}, host[128] = {0};
    if (credentials(key, host)) return NULL;
    char *payload = cJSON_PrintUnformatted(body);
    char *response = malloc(capacity);
    if (!payload || !response) {
        free(payload); free(response); memset(key, 0, sizeof(key)); return NULL;
    }
    vela_header_t headers[] = {{"api-key", key}, {NULL, NULL}};
    int status = vela_https_post_json(host, "443", "/v1/chat/completions", headers,
                                      payload, response, capacity);
    free(payload);
    cJSON *result = status == 200 ? cJSON_Parse(response) : NULL;
    printf("MIMOVOICE http=%d parsed=%d phase=%s elapsed_ms=%lld\n",
           status, result != NULL, phase, (long long)(monotonic_ms() - started));
    free(response);
    memset(key, 0, sizeof(key));
    return result;
}

static cJSON *message(cJSON *result)
{
    return cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(result, "choices"), 0), "message");
}

static void le16(unsigned char *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void le32(unsigned char *p, unsigned value) { le16(p, value); le16(p + 2, value >> 16); }

static int recognize(const unsigned char *pcm, size_t length, char *out, size_t capacity)
{
    if (!pcm || !length || length % 2 || length > 960000 || !out || !capacity) return -EINVAL;
    out[0] = 0;
    unsigned char *wav = calloc(1, length + 44);
    size_t b64cap = 4 * ((length + 46) / 3) + 1;
    char *encoded = malloc(b64cap + 22);
    if (!wav || !encoded) { free(wav); free(encoded); return -ENOMEM; }
    memcpy(wav, "RIFF", 4); le32(wav + 4, length + 36); memcpy(wav + 8, "WAVEfmt ", 8);
    le32(wav + 16, 16); le16(wav + 20, 1); le16(wav + 22, 1);
    le32(wav + 24, 16000); le32(wav + 28, 32000); le16(wav + 32, 2); le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); le32(wav + 40, length); memcpy(wav + 44, pcm, length);
    strcpy(encoded, "data:audio/wav;base64,");
    size_t written = 0;
    int rc = mbedtls_base64_encode((unsigned char *)encoded + 22, b64cap, &written, wav, length + 44);
    free(wav);
    if (rc) { free(encoded); return -EIO; }
    encoded[22 + written] = 0;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", "mimo-v2.5-asr");
    cJSON *messages = cJSON_AddArrayToObject(body, "messages"), *m = cJSON_CreateObject();
    cJSON_AddItemToArray(messages, m); cJSON_AddStringToObject(m, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(m, "content"), *audio = cJSON_CreateObject();
    cJSON_AddItemToArray(content, audio); cJSON_AddStringToObject(audio, "type", "input_audio");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(audio, "input_audio"), "data", encoded);
    free(encoded);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(body, "asr_options"), "language", "zh");
    printf("MIMOVOICE phase=asr begin audio_ms=%lu\n", (unsigned long)(length / 32));
    cJSON *result = request(body, 16384, "asr"); cJSON_Delete(body);
    if (!result) return -EIO;
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message(result), "content"));
    rc = !text ? -EIO : strlen(text) >= capacity ? -ENOSPC : 0;
    if (!rc) strcpy(out, text);
    cJSON_Delete(result);
    return rc;
}

static int fetch_speech(const char *text, unsigned char **pcm, size_t *length)
{
    if (!text || !text[0] || strlen(text) > 2048 || !pcm || !length) return -EINVAL;
    *pcm = NULL; *length = 0;
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", "mimo-v2.5-tts");
    cJSON *messages = cJSON_AddArrayToObject(body, "messages"), *m = cJSON_CreateObject();
    cJSON_AddItemToArray(messages, m); cJSON_AddStringToObject(m, "role", "assistant");
    cJSON_AddStringToObject(m, "content", text);
    cJSON *audio = cJSON_AddObjectToObject(body, "audio");
    cJSON_AddStringToObject(audio, "format", "pcm16");
    cJSON_AddStringToObject(audio, "voice", "mimo_default");
    printf("MIMOVOICE phase=tts begin text_bytes=%lu\n", (unsigned long)strlen(text));
    cJSON *result = request(body, 4 * 1024 * 1024, "tts"); cJSON_Delete(body);
    if (!result) return -EIO;
    const char *encoded = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(message(result), "audio"), "data"));
    int rc = -EIO;
    if (encoded) {
        size_t capacity = strlen(encoded) / 4 * 3 + 3;
        unsigned char *decoded = malloc(capacity);
        if (decoded && !mbedtls_base64_decode(decoded, capacity, length, (const unsigned char *)encoded, strlen(encoded)) && *length && !(*length % 2)) {
            *pcm = decoded; rc = 0;
        } else free(decoded);
    }
    cJSON_Delete(result);
    return rc;
}

static int synthesize(const char *text, unsigned char *out, size_t capacity, size_t *length)
{
    if (!out || !length) return -EINVAL;
    unsigned char *pcm; size_t bytes;
    int rc = fetch_speech(text, &pcm, &bytes); if (rc) return rc;
    /* Batch API contract is 16 kHz; MiMo PCM is 24 kHz mono. */
    size_t samples = bytes / 2, count = samples * 2 / 3;
    if (count * 2 > capacity) { free(pcm); return -ENOSPC; }
    const int16_t *in = (const int16_t *)pcm; int16_t *dst = (int16_t *)out;
    for (size_t i = 0; i < count; i++) {
        size_t at = i * 3 / 2;
        dst[i] = i % 2 && at + 1 < samples ? ((int)in[at] + in[at + 1]) / 2 : in[at];
    }
    *length = count * 2; free(pcm); return 0;
}

int zhaoxi_mimo_tts_stream(const char *text, voice_tts_chunk_cb callback, void *data)
{
    if (!callback) return -EINVAL;
    unsigned char *pcm; size_t length;
    int rc = fetch_speech(text, &pcm, &length); if (rc) return rc;
    /* Deliver decoded PCM in bounded playback chunks after this batch request. */
    for (size_t offset = 0; offset < length; offset += 3200) {
        size_t chunk = length - offset; if (chunk > 3200) chunk = 3200;
        callback(pcm + offset, chunk, offset + chunk == length, data);
    }
    free(pcm); return 0;
}

int zhaoxi_mimo_register(void)
{
    static const voice_asr_ops_t asr = {.name = "mimo", .recognize = recognize};
    static const voice_tts_ops_t tts = {.name = "mimo", .synthesize = synthesize};
    int rc = voice_asr_register(&asr); if (rc) return rc;
    return voice_tts_register(&tts);
}

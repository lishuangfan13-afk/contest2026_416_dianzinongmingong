/* SPDX-License-Identifier: Apache-2.0
 * MiMo ASR/TTS provider: official chat/completions audio protocol.
 * Credentials are read at runtime, never compiled into this source.
 */
#include "voice/voice_asr.h"
#include "voice/voice_tts.h"
#include "voice/voice_channel.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "mimo_sse.h"
#include "local_prompts.h"
#include "agent_compat.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
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

static int request_cancel(void *unused)
{(void)unused;return agent_shutdown_requested();}

static int stream_cancel(void *unused)
{(void)unused;return agent_shutdown_requested() || voice_channel_tts_stream_error();}

static cJSON *request(cJSON *body, size_t capacity, const char *phase,unsigned budget)
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
    int status = vela_https_post_json_bounded(host, "443", "/v1/chat/completions", headers,
                                      payload, response, capacity,budget,request_cancel,NULL);
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
    int64_t started=monotonic_ms();unsigned voiced=0,peak=0;
    for(size_t frame=0;frame+320<=length/2;frame+=320) {
        uint64_t sum=0;
        for(size_t i=frame;i<frame+320;i++){int16_t sample;memcpy(&sample,pcm+i*2,2);int v=sample;unsigned magnitude=(unsigned)(v<0?-v:v);sum+=magnitude;if(magnitude>peak)peak=magnitude;}
        if(sum/320>=300)voiced++;
    }
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
    printf("MIMOVOICE phase=asr begin audio_ms=%lu voiced_frames=%u peak=%u\n", (unsigned long)(length / 32),voiced,peak);
    cJSON *result = request(body, 16384, "asr",20000);
    if (!result){cJSON_Delete(body);return -EIO;}
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message(result), "content"));
    /* An empty transcript is not network success or proof of silence. Only
     * retry a valid, non-quiet clip once, inside a shared 25-second budget. */
    int64_t remaining=25000-(monotonic_ms()-started);
    if(text && !text[0] && voiced>=8 && peak>=600 && remaining>=5000 && !agent_shutdown_requested()) {
        cJSON_Delete(result);
        printf("MIMOVOICE phase=asr retry=empty-text budget_ms=%lld\n",(long long)remaining);
        result=request(body,16384,"asr",(unsigned)remaining);
        text=cJSON_GetStringValue(cJSON_GetObjectItem(message(result),"content"));
    }
    cJSON_Delete(body);
    rc = !text ? -EIO : !text[0]?-ENODATA:strlen(text) >= capacity ? -ENOSPC : 0;
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
    cJSON *result = request(body, 4 * 1024 * 1024, "tts",60000); cJSON_Delete(body);
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

struct stream_output {
    voice_tts_chunk_cb callback;
    void *user;
    int64_t started, first_pcm;
};

static int stream_pcm(const unsigned char *pcm, size_t length, void *opaque)
{
    struct stream_output *out = opaque;
    int rc = voice_channel_tts_stream_error();
    if (rc) return rc;
    if (out->first_pcm < 0) {
        out->first_pcm = monotonic_ms() - out->started;
        printf("MIMOVOICE phase=tts first_pcm_ms=%lld\n", (long long)out->first_pcm);
    }
    out->callback(pcm, length, 0, out->user);
    return voice_channel_tts_stream_error();
}

static int stream_body(const unsigned char *bytes, size_t length, void *opaque)
{
    int rc = voice_channel_tts_stream_error();
    return rc ? rc : mimo_sse_feed(opaque, bytes, length);
}

static int local_pcm(const unsigned char *pcm,size_t length,void *opaque)
{
    struct stream_output *out=opaque;
    if(agent_shutdown_requested())return -ECANCELED;
    int rc=voice_channel_tts_stream_error();if(rc)return rc;
    out->callback(pcm,length,0,out->user);
    return voice_channel_tts_stream_error();
}
int zhaoxi_mimo_tts_stream(const char *text, voice_tts_chunk_cb callback, void *data)
{
    if (!callback || !text || !text[0] || strlen(text) > 2048) return -EINVAL;
    const char *prompt=zhaoxi_prompt_id(text);
    if(prompt) {
        bool handled=false;
        struct stream_output local={callback,data,monotonic_ms(),-1};
        int local_result=zhaoxi_prompt_stream(prompt,local_pcm,request_cancel,&local,&handled);
        if(handled || agent_shutdown_requested()) {
            if(!local_result){callback(NULL,0,1,data);local_result=voice_channel_tts_stream_error();}
            return local_result;
        }
    }
    char key[128] = {0}, host[128] = {0};
    int rc = credentials(key, host);
    if (rc) return rc;
    cJSON *body = cJSON_CreateObject();
    if (!body) { memset(key, 0, sizeof(key)); return -ENOMEM; }
    cJSON_AddStringToObject(body, "model", "mimo-v2.5-tts");
    cJSON_AddBoolToObject(body, "stream", true);
    cJSON *messages = cJSON_AddArrayToObject(body, "messages"), *m = cJSON_CreateObject();
    cJSON_AddItemToArray(messages, m); cJSON_AddStringToObject(m, "role", "assistant");
    cJSON_AddStringToObject(m, "content", text);
    cJSON *audio = cJSON_AddObjectToObject(body, "audio");
    cJSON_AddStringToObject(audio, "format", "pcm16");
    cJSON_AddStringToObject(audio, "voice", "mimo_default");
    char *payload = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    struct stream_output output = {callback, data, monotonic_ms(), -1};
    mimo_sse_t *sse = mimo_sse_create(stream_pcm, &output);
    if (!payload || !sse) {
        free(payload); mimo_sse_destroy(sse); memset(key, 0, sizeof(key)); return -ENOMEM;
    }
    vela_header_t headers[] = {{"api-key", key}, {"Accept", "text/event-stream"}, {NULL, NULL}};
    printf("MIMOVOICE phase=tts begin mode=stream text_bytes=%lu\n", (unsigned long)strlen(text));
    int status = vela_https_post_json_stream_bounded(host, "443", "/v1/chat/completions", headers,
        payload, stream_body, sse,180000,stream_cancel,NULL);
    free(payload); memset(key, 0, sizeof(key));
    rc = status == 200 ? mimo_sse_finish(sse) : status < 0 ? status : -EIO;
    if (!rc) {
        callback(NULL, 0, 1, data);
        rc = voice_channel_tts_stream_error();
    }
    printf("MIMOVOICE phase=tts mode=stream http=%d rc=%d first_pcm_ms=%lld total_ms=%lld pcm_bytes=%lu\n",
        status, rc, (long long)output.first_pcm, (long long)(monotonic_ms() - output.started),
        (unsigned long)mimo_sse_pcm_bytes(sse));
    mimo_sse_destroy(sse);
    return rc;
}

int zhaoxi_mimo_register(void)
{
    static const voice_asr_ops_t asr = {.name = "mimo", .recognize = recognize};
    static const voice_tts_ops_t tts = {.name = "mimo", .synthesize = synthesize};
    int rc = voice_asr_register(&asr); if (rc) return rc;
    return voice_tts_register(&tts);
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "reminder.h"
#include "agent_config.h"
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include "infra/network_manager.h"
#include "voice/audio_playback.h"
#include "voice/voice_channel.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int play_alert_pcm(void *context, void (*started)(void))
{
    (void)context;
    /* Fixed 16 kHz, two-second alert at its previously selected amplitude;
     * speech can use the provider's full PCM range without raising this tone. */
    static const int16_t wave[16] = {0, 12537, 23165, 30266, 32760, 30266,
        23165, 12537, 0, -12537, -23165, -30266, -32760, -30266, -23165, -12537};
    int16_t *pcm = malloc(3200);
    if (!pcm) return -ENOMEM;
    for (int i = 0; i < 1600; i++) pcm[i] = wave[i % 16] / 2;
    audio_playback_t *playback = audio_playback_open(NULL, 16000, 1, 16);
    int result = playback ? 0 : -(errno ? errno : EIO);
    for (int i = 0; playback && !result && i < 20; i++) {
        int written = audio_playback_write(playback, pcm, 3200);
        if (written != 3200) result = written < 0 ? written : -EIO;
        else if (i == 0 && started) started();
    }
    if (playback) audio_playback_close(playback);
    free(pcm);
    return result;
}

int zhaoxi_play_alert(void)
{
    /* Local fallback alerts share the same capture exclusion and output
     * serialization as cloud speech, including every error return. */
    return voice_channel_play_local(play_alert_pcm, NULL);
}

int zhaoxi_deliver_reminder(const char *text)
{
    if (!text || !text[0]) return -EINVAL;
    printf("\n[Reminder]: %s\n", text);
    fflush(stdout);
    int speech_result = -ENETDOWN;
    if (network_is_connected()) speech_result = voice_channel_speak(text);
    if (!speech_result) {
        puts("REMINDER_DELIVERY mode=speech rc=0 speech_played=1");
        return 0;
    }
    int result = zhaoxi_play_alert();
    printf("REMINDER_DELIVERY mode=local_tone rc=%d speech_played=0 speech_rc=%d\n",
           result, speech_result);
    fflush(stdout);
    return result;
}

static int reminder_list(void)
{
    cron_job_t *jobs = calloc(AGENT_CRON_MAX_JOBS, sizeof(*jobs));
    if (!jobs) return 1;
    int count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);
    if (count < 0 || count > AGENT_CRON_MAX_JOBS) { free(jobs); return 1; }
    printf("本机提醒 %d 条：\n", count);
    for (int i = 0; i < count; i++) {
        printf("%s enabled=%d next_epoch=%lld %s\n", jobs[i].id, jobs[i].enabled,
               (long long)jobs[i].next_run, jobs[i].message);
    }
    free(jobs);
    return 0;
}

int zhaoxi_reminder_create(unsigned delay, const char *text, const char *channel,
                          struct zhaoxi_reminder_result *result, char **reply)
{
    if (!result || !reply) return -EINVAL;
    memset(result, 0, sizeof(*result)); *reply = NULL;
    if (!message_bus_is_ready()) return -ENOTCONN;
    if (!delay || delay > 31536000 || !text || !*text || !channel ||
        (strcmp(channel, "voice") && strcmp(channel, "cli"))) return -EINVAL;
    cron_job_t job = {0};
    size_t size = strlen(text);
    if (size >= sizeof(job.message)) return -E2BIG;
    for (size_t i = 0; i < size; i++)
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] == 127) return -EINVAL;
    time_t now = time(NULL);
    if ((int64_t)now < 1735689600LL || (int64_t)now > 4102444800LL - delay) return -EINVAL;
    char *answer = malloc(512);
    cron_job_t *jobs = calloc(AGENT_CRON_MAX_JOBS, sizeof(*jobs));
    if (!answer || !jobs) { free(answer); free(jobs); return -ENOMEM; }
    strcpy(job.message, text); strcpy(job.name, "本地提醒");
    strcpy(job.chat_id, "zhaoxi");
    snprintf(job.channel, sizeof(job.channel), "%s", channel);
    job.kind = CRON_KIND_AT;
    job.at_epoch = (int64_t)now + delay;
    job.delete_after_run = true;
    int rc = cron_add_job(&job);
    if (!rc) {
        result->storage_uncertain = 1;
        snprintf(result->id, sizeof(result->id), "%s", job.id);
        result->at_epoch = job.at_epoch;
        int count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);
        int matched = 0;
        for (int i = 0; count >= 0 && i < count && i < AGENT_CRON_MAX_JOBS; i++) {
            if (!strcmp(jobs[i].id, job.id) && jobs[i].kind == CRON_KIND_AT &&
                jobs[i].at_epoch == job.at_epoch && jobs[i].delete_after_run && jobs[i].enabled &&
                !strcmp(jobs[i].channel, channel) && !strcmp(jobs[i].chat_id, "zhaoxi") &&
                !strcmp(jobs[i].message, text)) matched++;
        }
        if (count < 0 || count > AGENT_CRON_MAX_JOBS || matched != 1) rc = -EIO;
    }
    if (!rc) {
        result->verified = 1; result->storage_uncertain = 0;
        snprintf(answer, 512, "已设置%u秒后的提醒：%s", delay, text);
        *reply = answer;
    } else free(answer);
    free(jobs);
    return rc;
}

int zhaoxi_reminder_command(int argc, char **argv, int at, const char *channel)
{
    if (!message_bus_is_ready()) {
        puts("Agent 尚未启动或正在停止，提醒操作未执行。");
        return 1;
    }
    if (at + 1 >= argc) return 1;
    const char *operation = argv[at + 1];
    if (!strcmp(operation, "list") && at + 2 == argc) return reminder_list();
    if (!strcmp(operation, "cancel") && at + 3 == argc) {
        const char *id = argv[at + 2];
        if (strlen(id) != 8 || strspn(id, "0123456789abcdefABCDEF") != 8) {
            puts("请提供完整的八位提醒编号。");
            return 1;
        }
        int result = cron_remove_job(id);
        puts(result ? "未取消：编号不存在或保存失败。" : "提醒已取消并保存。");
        return result ? 1 : 0;
    }
    if (strcmp(operation, "in") || at + 3 >= argc) return 1;
    const char *seconds = argv[at + 2];
    if (!seconds[0] || strspn(seconds, "0123456789") != strlen(seconds)) return 1;
    errno = 0;
    char *end = NULL;
    unsigned long delay = strtoul(seconds, &end, 10);
    /* Explicit relative scheduling is bounded to one year; natural-language
     * dates and recurrence remain available through the existing Agent. */
    if (errno || !end || *end || !delay || delay > 31536000UL) {
        puts("提醒秒数需要在 1 至 31536000 之间。");
        return 1;
    }
    char text[256] = "";
    size_t used = 0;
    for (int i = at + 3; i < argc; i++) {
        size_t n = strlen(argv[i]), separator = i > at + 3 ? 1 : 0;
        if (!n || n + separator >= sizeof(text) - used) return 1;
        for (size_t j = 0; j < n; j++) if ((unsigned char)argv[i][j] < 32) return 1;
        if (separator) text[used++] = ' ';
        memcpy(text + used, argv[i], n + 1);
        used += n;
    }
    struct zhaoxi_reminder_result result;
    char *reply = NULL;
    int rc = zhaoxi_reminder_create((unsigned)delay, text, channel, &result, &reply);
    free(reply);
    if (rc) {
        puts("提醒尚未确认保存，请检查时钟和存储，先查询再决定是否重试。");
        return 1;
    }
    printf("提醒已保存 id=%s at_epoch=%lld channel=%s 内容：%s\n",
           result.id, (long long)result.at_epoch, channel, text);
    return 0;
}

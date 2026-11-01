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

int zhaoxi_play_alert(void)
{
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
    }
    if (playback) audio_playback_close(playback);
    free(pcm);
    return result;
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
    time_t now = time(NULL);
    if (now < 1735689600LL || (int64_t)now + delay > 4102444800LL) {
        puts("系统时间尚未校准，未创建提醒。");
        return 1;
    }
    cron_job_t job = {0};
    size_t used = 0;
    for (int i = at + 3; i < argc; i++) {
        size_t n = strlen(argv[i]), separator = i > at + 3 ? 1 : 0;
        if (!n || n + separator >= sizeof(job.message) - used) return 1;
        for (size_t j = 0; j < n; j++) if ((unsigned char)argv[i][j] < 32) return 1;
        if (separator) job.message[used++] = ' ';
        memcpy(job.message + used, argv[i], n + 1);
        used += n;
    }
    strcpy(job.name, "本地提醒");
    strcpy(job.chat_id, "zhaoxi");
    snprintf(job.channel, sizeof(job.channel), "%s", channel);
    job.kind = CRON_KIND_AT;
    job.at_epoch = (int64_t)now + delay;
    job.delete_after_run = true;
    if (cron_add_job(&job) != 0) {
        puts("提醒未创建，保存失败或任务容量已满。");
        return 1;
    }
    printf("提醒已保存 id=%s at_epoch=%lld channel=%s 内容：%s\n",
           job.id, (long long)job.at_epoch, job.channel, job.message);
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "drink.h"
#include "agent_config.h"
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DRINK_OWNER "zhaoxi_drink"
#define DRINK_REPLY_SIZE 256
static pthread_mutex_t drink_lock = PTHREAD_MUTEX_INITIALIZER;

static int owned(const cron_job_t *job)
{
    return job->kind == CRON_KIND_EVERY && !job->action[0] &&
           !strcmp(job->chat_id, DRINK_OWNER);
}

static int read_jobs(cron_job_t *jobs, unsigned *matched)
{
    int count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);
    if (count < 0 || count > AGENT_CRON_MAX_JOBS) return -EIO;
    *matched = 0;
    for (int i = 0; i < count; i++) if (owned(&jobs[i])) (*matched)++;
    return count;
}

int zhaoxi_drink_execute(const char *op, unsigned minutes, const char *channel,
                         struct zhaoxi_drink_result *result, char **reply)
{
    if (!result || !reply) return -EINVAL;
    memset(result, 0, sizeof(*result)); *reply = NULL;
    if (!op || !channel || (strcmp(channel, "voice") && strcmp(channel, "cli"))) return -EINVAL;
    int starting = !strcmp(op, "start"), stopping = !strcmp(op, "stop");
    int status = !strcmp(op, "status");
    if ((!starting && !stopping && !status) || (starting && (!minutes || minutes > 1440))) return -EINVAL;
    char *answer = calloc(1, DRINK_REPLY_SIZE);
    cron_job_t *jobs = calloc(AGENT_CRON_MAX_JOBS, sizeof(*jobs));
    if (!answer || !jobs) { free(answer); free(jobs); return -ENOMEM; }
    int rc = 0;
    if (!message_bus_is_ready()) { rc = -ENOTCONN; goto done_unlocked; }
    time_t now = time(NULL);
    if (starting && ((long long)now < 1735689600LL ||
                     (long long)now > 4102444800LL - (long long)minutes * 60)) {
        snprintf(answer, DRINK_REPLY_SIZE, "系统时间尚未校准，未开启喝水提醒。");
        rc = -EINVAL; goto done_unlocked;
    }
    int lock_rc = pthread_mutex_lock(&drink_lock);
    if (lock_rc) { rc = -lock_rc; goto done_unlocked; }
    unsigned matched = 0;
    int count = read_jobs(jobs, &matched);
    if (count < 0) { rc = count; goto done; }
    result->count = matched;
    if (starting) {
        if (matched) {
            for (int i = 0; i < count; i++) if (owned(&jobs[i])) {
                if (matched != 1 || !jobs[i].enabled ||
                    jobs[i].interval_s != minutes * 60 || strcmp(jobs[i].channel, channel)) {
                    snprintf(answer, DRINK_REPLY_SIZE,
                             "已有喝水提醒；更改间隔或播报方式时，请先关闭喝水提醒。");
                    rc = -EEXIST; goto done;
                }
                snprintf(result->id, sizeof(result->id), "%s", jobs[i].id);
                result->interval_s = jobs[i].interval_s; result->next_epoch = jobs[i].next_run;
                result->existing = 1; result->enabled = 1; result->verified = 1;
                snprintf(answer, DRINK_REPLY_SIZE, "喝水提醒已开启，每 %u 分钟提醒一次。", minutes);
                goto done;
            }
        }
        cron_job_t job = {0};
        strcpy(job.name, "喝水提醒");
        strcpy(job.message, "该喝水了，休息一下再继续。");
        strcpy(job.chat_id, DRINK_OWNER);
        snprintf(job.channel, sizeof(job.channel), "%s", channel);
        job.kind = CRON_KIND_EVERY;
        job.interval_s = minutes * 60;
        if (cron_add_job(&job)) { rc = -EIO; goto done; }
        snprintf(result->id, sizeof(result->id), "%s", job.id);
        result->interval_s = job.interval_s; result->next_epoch = job.next_run;
        count = read_jobs(jobs, &matched);
        result->count = matched;
        int verified = 0;
        for (int i = 0; count >= 0 && i < count; i++) {
            if (owned(&jobs[i]) && !strcmp(jobs[i].id, job.id) && jobs[i].enabled &&
                jobs[i].interval_s == job.interval_s && !strcmp(jobs[i].channel, channel) &&
                !strcmp(jobs[i].message, job.message)) {
                verified++;
                result->next_epoch = jobs[i].next_run;
            }
        }
        if (count < 0 || matched != 1 || verified != 1) { rc = -EIO; goto done; }
        result->enabled = 1; result->verified = 1;
        snprintf(answer, DRINK_REPLY_SIZE, "已开启喝水提醒，每 %u 分钟提醒一次。", minutes);
    } else if (stopping) {
        for (int i = 0; i < count; i++) if (owned(&jobs[i])) {
            if (cron_remove_job(jobs[i].id)) { rc = -EIO; goto done; }
            result->removed++;
        }
        count = read_jobs(jobs, &matched);
        result->count = matched;
        if (count < 0 || matched) { rc = -EIO; goto done; }
        result->verified = 1;
        snprintf(answer, DRINK_REPLY_SIZE, "%s", result->removed ? "喝水提醒已关闭。" : "喝水提醒当前已关闭。");
    } else {
        result->verified = 1;
        if (!matched) snprintf(answer, DRINK_REPLY_SIZE, "喝水提醒当前已关闭。");
        else if (matched != 1) snprintf(answer, DRINK_REPLY_SIZE,
                                      "当前有 %u 条喝水提醒，请先关闭后重新开启。", matched);
        else for (int i = 0; i < count; i++) if (owned(&jobs[i])) {
            snprintf(result->id, sizeof(result->id), "%s", jobs[i].id);
            result->interval_s = jobs[i].interval_s; result->next_epoch = jobs[i].next_run; result->enabled = jobs[i].enabled;
            snprintf(answer, DRINK_REPLY_SIZE, "喝水提醒%s，间隔 %u 分钟。",
                     jobs[i].enabled ? "已开启" : "已暂停", (unsigned)jobs[i].interval_s / 60);
        }
    }
done:
    pthread_mutex_unlock(&drink_lock);
done_unlocked:
    free(jobs);
    if (rc) {
        result->verified = 0;
        if (!answer[0]) snprintf(answer, DRINK_REPLY_SIZE, "喝水提醒操作未完成，请先查询当前设置。");
    }
    *reply = answer;
    return rc;
}

static void deliver_reply(const char *channel, char *text)
{
    puts(text);
    if (strcmp(channel, "voice")) { free(text); return; }
    agent_msg_t message = {0};
    strcpy(message.channel, "voice"); strcpy(message.chat_id, "zhaoxi");
    message.content = text;
    int rc = message_bus_push_outbound(&message);
    if (rc) message_bus_msg_free(&message);
    printf("DRINK_REPLY_QUEUED rc=%d\n", rc);
}

int zhaoxi_drink_command(int argc, char **argv, int at, const char *channel)
{
    if (!channel || at + 1 >= argc || !message_bus_is_ready()) return 1;
    const char *op = argv[at + 1];
    int starting = !strcmp(op, "start"), stopping = !strcmp(op, "stop");
    int status = !strcmp(op, "status");
    unsigned long minutes = 60;
    if ((!starting && !stopping && !status) ||
        (starting ? argc != at + 2 && argc != at + 3 : argc != at + 2)) return 1;
    if (starting && argc == at + 3) {
        const char *value = argv[at + 2];
        if (!value[0] || strspn(value, "0123456789") != strlen(value)) return 1;
        errno = 0; char *end = NULL;
        minutes = strtoul(value, &end, 10);
        if (errno || !end || *end || !minutes || minutes > 1440) return 1;
    }
    struct zhaoxi_drink_result result;
    char *answer = NULL;
    int rc = zhaoxi_drink_execute(op, (unsigned)minutes, channel, &result, &answer);
    if (rc) {
        if (stopping) printf("DRINK_RESULT op=stop rc=%d removed=%u\n", rc, result.removed);
        printf("DRINK_ERROR op=%s rc=%d\n", op, rc);
        if (answer) { puts(answer); free(answer); }
        return 1;
    }
    if (starting) printf("DRINK_RESULT op=start rc=0 id=%s interval_s=%u existing=%d\n",
                         result.id, result.interval_s, result.existing);
    else if (stopping) printf("DRINK_RESULT op=stop rc=0 removed=%u\n", result.removed);
    else {
        printf("DRINK_RESULT op=status rc=0 count=%u\n", result.count);
        if (result.id[0]) printf("DRINK_JOB id=%s enabled=%d interval_s=%u next_epoch=%lld\n",
                                 result.id, result.enabled, result.interval_s, (long long)result.next_epoch);
    }
    deliver_reply(channel, answer);
    return 0;
}

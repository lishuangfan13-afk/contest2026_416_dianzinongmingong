/* SPDX-License-Identifier: Apache-2.0 */
/* Actual SDK implementation; only task creation, time and message delivery
 * are controlled. No microphone, network or real filesystem outside tmpfs. */
#include "infra/cron_service.h"
#include "tools/tool_cron.h"
#include "core/message_bus.h"
#include "agent_config.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static time_t test_now = 1793536553;
static unsigned emitted;
static int outbound_full;
#ifdef CONFIG_ZHAOXI_BES_AUDIO
static unsigned notified;
static int64_t notified_due;
static char notified_id[32];
int zhaoxi_notify_reminder_event(const char *id,int64_t due,const char *text)
{
    assert(emitted && text && *text);notified++;notified_due=due;
    snprintf(notified_id,sizeof(notified_id),"%s",id);
    return -ENOSPC; /* Remote queue failure must not replay local delivery. */
}
#endif
time_t __wrap_time(time_t *out)
{ if (out) *out = test_now; return test_now; }
int agent_task_create(void *(*fn)(void *), const char *name, int stack, void *arg, int priority)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; return 0; }
int tool_registry_execute(const char *name, const char *json, char *out, size_t size)
{ (void)name; (void)json; (void)out; (void)size; assert(0); return -1; }
int message_bus_push_outbound(const agent_msg_t *message)
{ assert(message && message->content);if(outbound_full)return -ENOSPC;emitted++;free(message->content);return 0; }

#include "infra/cron_service.c"
#include "tools/tool_cron.c"

static void write_text(const char *text)
{
    FILE *file = fopen(AGENT_CRON_FILE, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void reset_store(void)
{
    cron_service_stop();
    unlink(AGENT_CRON_FILE ".tmp");
    unlink(AGENT_CRON_FILE ".bak");
    write_text("{\"jobs\":[]}");
    assert(cron_service_init() == 0);
    test_now = 1793536553;
    emitted = 0;
    outbound_full=0;
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    notified=0;
#endif
}

static cron_job_t only_job(void)
{
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 1);
    return jobs[0];
}

static cron_job_t recurring(unsigned interval)
{
    cron_job_t job = {0};
    strcpy(job.name, "recurring");
    strcpy(job.message, "real reminder");
    strcpy(job.channel, "voice");
    strcpy(job.chat_id, "zhaoxi");
    job.kind = CRON_KIND_EVERY;
    job.interval_s = interval;
    return job;
}

static int add_numeric(const char *type, const char *key, const char *value)
{
    char input[768], output[512];
    snprintf(input, sizeof(input), "{\"name\":\"number test\",\"schedule_type\":\"%s\",\"%s\":%s,\"message\":\"number test\",\"channel\":\"voice\",\"chat_id\":\"zhaoxi\"}", type, key, value);
    return tool_cron_add_execute(input, output, sizeof(output));
}

static void assert_empty(void)
{
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 0);
}

static void numeric_parameters(void)
{
    reset_store();
    const char *bad[] = {"0", "-1", "0.5", "1.5", "4294967296", "1e100",
        "\"nan\"", "\"inf\"", "\"-inf\"", "\"0.5\"", "\"4294967296\"", "null", "true"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        assert(add_numeric("every", "interval_s", bad[i]) != 0);
        assert_empty();
    }
    const char *good[] = {"1", "60", "4294967295", "\"30\""};
    const uint32_t expected[] = {1, 60, UINT32_MAX, 30};
    for (unsigned i = 0; i < sizeof(good)/sizeof(good[0]); i++) {
        assert(add_numeric("every", "interval_s", good[i]) == 0);
        cron_job_t job = only_job();
        assert(job.interval_s == expected[i] && job.enabled);
        assert(job.next_run == (int64_t)test_now + expected[i]);
        assert(cron_remove_job(job.id) == 0);
    }
    const char *bad_epoch[] = {"1e100", "\"nan\"", "\"inf\"", "9223372036854775808", "2000000000.5"};
    for (unsigned i = 0; i < sizeof(bad_epoch)/sizeof(bad_epoch[0]); i++)
        assert(add_numeric("at", "at_epoch", bad_epoch[i]) != 0);
    assert(add_numeric("at", "at_epoch", "\"2000000000\"") == 0);
    cron_job_t job = only_job();
    assert(job.at_epoch == 2000000000LL);
    assert(cron_remove_job(job.id) == 0);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    job = recurring(0);
    assert(cron_add_job(&job) != 0 && !job.id[0]);
    job = recurring(10);
    test_now = INT64_MAX - 2;
    assert(cron_add_job(&job) != 0 && !job.id[0]);
    assert_empty();
#else
    /* Internal standard-profile callers keep their pre-existing behavior. */
    job = recurring(0);
    assert(cron_add_job(&job) == 0);
    assert(cron_remove_job(job.id) == 0);
#endif
    test_now = 1793536553;
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    test_now = 123;
    assert(add_numeric("every", "interval_s", "60") != 0);
    assert(add_numeric("at", "at_epoch", "2000000000") != 0);
#endif
}

static void absolute_epoch_does_not_shift_with_timezone(void)
{
    const char *zones[] = {"UTC0", "CST-8"};
    for (unsigned i = 0; i < sizeof(zones)/sizeof(zones[0]); i++) {
        reset_store();
        assert(setenv("TZ", zones[i], 1) == 0);
        tzset();
        assert(add_numeric("at", "at_epoch", "2000000000") == 0);
        cron_job_t job = only_job();
        assert(job.at_epoch == 2000000000LL && job.next_run == 2000000000LL);
        assert(cron_remove_job(job.id) == 0);
    }
}

static void restart_recurring(void)
{
    reset_store();
    int64_t initial = test_now;
    cron_job_t job = recurring(60);
    assert(cron_add_job(&job) == 0);
    assert(cron_service_init() == 0);
    test_now += 120;
    assert(cron_service_start() == 0);
    job = only_job();
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert(job.enabled && job.next_run == initial + 180);
    cron_process_due_jobs();
    assert(emitted == 0);
    test_now = job.next_run;
    cron_process_due_jobs();
    assert(emitted == 1 && only_job().next_run == test_now + 60);
    cron_process_due_jobs();
    assert(emitted == 1);
#else
    (void)initial;
    assert(!job.enabled && job.next_run == 0);
#endif
    cron_service_stop();
    assert(cron_service_init() == 0);
    job = only_job();
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert(job.enabled);
#else
    assert(!job.enabled);
#endif
}

static void missed_one_shot_and_action_unchanged(void)
{
    reset_store();
    cron_job_t job = recurring(60);
    job.kind = CRON_KIND_AT;
    job.at_epoch = test_now + 60;
    job.delete_after_run = true;
    assert(cron_add_job(&job) == 0);
    job = recurring(60);
    strcpy(job.action, "do_not_replay");
    assert(cron_add_job(&job) == 0);
    test_now += 120;
    assert(cron_service_start() == 0);
    cron_job_t jobs[2];
    assert(cron_list_jobs(jobs, 2) == 2);
    assert(!jobs[0].enabled && !jobs[1].enabled);
    cron_process_due_jobs();
    assert(emitted == 0);
    cron_service_stop();
}

static void failed_resume_rolls_back(void)
{
    reset_store();
    cron_job_t job = recurring(60);
    assert(cron_add_job(&job) == 0);
    int64_t old_next = job.next_run;
    test_now += 120;
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    assert(cron_service_start() != 0 && !s_cron_running);
    job = only_job();
    assert(job.enabled && job.next_run == old_next);
    assert(cron_service_init() == 0);
    job = only_job();
    assert(job.enabled && job.next_run == old_next);
    assert(cron_service_start() == 0);
    cron_service_stop();
}

#ifdef CONFIG_EXAMPLES_ZHAOXI
static void untrusted_clock_waits_for_sync(void)
{
    reset_store();
    int64_t initial = test_now;
    cron_job_t job = recurring(60);
    assert(cron_add_job(&job) == 0);
    int64_t old_next = job.next_run;
    test_now = 123;
    assert(cron_service_start() == 0 && s_waiting_for_clock);
    cron_process_due_jobs();
    assert(emitted == 0 && only_job().next_run == old_next);
    test_now = initial + 180;
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    cron_process_due_jobs();
    assert(s_waiting_for_clock && emitted == 0);
    assert(only_job().enabled && only_job().next_run == old_next);
    cron_process_due_jobs();
    assert(!s_waiting_for_clock && emitted == 0);
    assert(only_job().next_run == initial + 240);
    test_now = only_job().next_run;
    cron_process_due_jobs();
    assert(emitted == 1);
    cron_service_stop();
}

static void resumed_epoch_overflow_is_bounded(void)
{
    reset_store();
    cron_job_t job = recurring(60);
    assert(cron_add_job(&job) == 0);
    test_now = INT64_MAX - 1;
    assert(cron_service_start() == 0);
    job = only_job();
    assert(!job.enabled && !job.next_run);
    cron_service_stop();
}

static void legacy_zero_interval_is_disabled(void)
{
    reset_store();
    write_text("{\"jobs\":[{\"id\":\"0000abcd\",\"name\":\"legacy zero\",\"kind\":\"every\",\"interval_s\":0,\"message\":\"legacy zero\",\"channel\":\"voice\",\"chat_id\":\"zhaoxi\",\"enabled\":true,\"next_run\":1}]}");
    assert(cron_service_init() == 0 && cron_service_start() == 0);
    cron_job_t job = only_job();
    assert(!job.enabled && !job.next_run);
    cron_process_due_jobs();
    assert(emitted == 0);
    cron_service_stop();
}
static void notification_follows_local_acceptance(void)
{
    reset_store();cron_job_t job=recurring(60);
    assert(!cron_add_job(&job));assert(!cron_service_start());
    test_now=only_job().next_run;outbound_full=1;cron_process_due_jobs();
    assert(!emitted && !notified && only_job().next_run==test_now);
    outbound_full=0;cron_process_due_jobs();
    assert(emitted==1 && notified==1 && notified_due==test_now && !strcmp(notified_id,job.id));
    assert(only_job().next_run==test_now+60);cron_process_due_jobs();assert(notified==1);
    test_now+=60;cron_process_due_jobs();assert(emitted==2 && notified==2 && notified_due==test_now);
    cron_service_stop();
}
#endif

int main(void)
{
    assert(mkdir(AGENT_DATA_DIR, 0700) == 0);
    numeric_parameters();
    absolute_epoch_does_not_shift_with_timezone();
    restart_recurring();
    missed_one_shot_and_action_unchanged();
    failed_resume_rolls_back();
#ifdef CONFIG_EXAMPLES_ZHAOXI
    untrusted_clock_waits_for_sync();
    resumed_epoch_overflow_is_bounded();
    legacy_zero_interval_is_disabled();
    notification_follows_local_acceptance();
#endif
    puts("PASS: actual reminder parameters, restart policy, persistence rollback, cold clock and bounded recurrence.");
    return 0;
}

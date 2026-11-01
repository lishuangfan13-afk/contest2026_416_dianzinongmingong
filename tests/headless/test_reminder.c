/* SPDX-License-Identifier: Apache-2.0 */
#include "reminder.h"
#include "agent_config.h"
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include "voice/audio_playback.h"
#include "voice/voice_channel.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Hardware/network doubles; scheduler, persistence and message queues are real. */
static bool connected;
static int speech_result, speech_calls, writes, closes, fail_write, fail_open;
static pthread_t scheduler;
static int audio_token;
static time_t fake_time;
static int local_scopes, local_started;
static void mark_local_started(void) { local_started++; }
int voice_channel_play_local(voice_local_playback_cb callback, void *context)
{ assert(callback); local_scopes++; return callback(context, mark_local_started); }
time_t __real_time(time_t *out);
time_t __wrap_time(time_t *out)
{
    if (!fake_time) return __real_time(out);
    if (out) *out = fake_time;
    return fake_time;
}
bool network_is_connected(void) { return connected; }
int voice_channel_speak(const char *text)
{ assert(text && *text); speech_calls++; return speech_result; }
audio_playback_t *audio_playback_open(const char *path, unsigned rate, unsigned ch, unsigned bits)
{
    (void)path;
    assert(rate == 16000 && ch == 1 && bits == 16);
    if (fail_open) { errno = EBUSY; return NULL; }
    return (audio_playback_t *)&audio_token;
}
int audio_playback_write(audio_playback_t *pb, const void *buf, size_t n)
{
    assert(pb == (audio_playback_t *)&audio_token && buf && n == 3200);
    writes++;
    return fail_write ? -EIO : (int)n;
}
void audio_playback_close(audio_playback_t *pb)
{ assert(pb == (audio_playback_t *)&audio_token); closes++; }
int agent_task_create(void *(*fn)(void *), const char *name, int size, void *arg, int prio)
{
    (void)name; (void)size; (void)prio;
    return pthread_create(&scheduler, NULL, fn, arg);
}
int tool_registry_execute(const char *name, const char *input, char *out, size_t cap)
{ (void)name; (void)input; (void)out; (void)cap; assert(0); return -1; }

static int command(int count, char **args, const char *channel)
{ return zhaoxi_reminder_command(count, args, 1, channel); }

int main(void)
{
    assert(mkdir(AGENT_DATA_DIR, 0700) == 0);
    assert(message_bus_init() == 0);
    assert(cron_service_init() == 0);
    char *add[] = {"zhaoxi", "remind", "in", "1", "喝水"};
    char *list[] = {"zhaoxi", "remind", "list"};
    char *cancel[] = {"zhaoxi", "remind", "cancel", "12345678"};
    fake_time = 123;
    assert(command(5, add, "voice") != 0);
    fake_time = 0;
    for (unsigned i = 0; i < 5; i++) {
        char *bad[] = {"0", "-1", "+1", "1.5", "31536001"};
        add[3] = bad[i];
        assert(command(5, add, "voice") != 0);
    }
    add[3] = "3600";
    assert(command(5, add, "voice") == 0);
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 1);
    assert(!strcmp(jobs[0].message, "喝水") && !strcmp(jobs[0].channel, "voice"));
    assert(jobs[0].at_epoch > time(NULL) && strlen(jobs[0].id) == 8);
    assert(command(3, list, "voice") == 0);
    cancel[3] = jobs[0].id;
    assert(command(4, cancel, "voice") == 0);
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 0);
    assert(command(4, cancel, "voice") != 0);
    char huge[300]; memset(huge, 'x', sizeof(huge) - 1); huge[sizeof(huge) - 1] = 0;
    add[4] = huge;
    assert(command(5, add, "voice") != 0);
    add[4] = "control\ntext";
    assert(command(5, add, "voice") != 0);
    add[4] = "do not confirm failed persistence";
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    assert(command(5, add, "voice") != 0);
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 0);

    assert(zhaoxi_deliver_reminder("offline reminder") == 0);
    assert(speech_calls == 0 && writes == 20 && closes == 1);
    assert(local_scopes == 1 && local_started == 1);
    connected = true;
    assert(zhaoxi_deliver_reminder("spoken reminder") == 0);
    assert(speech_calls == 1 && writes == 20);
    speech_result = -EIO;
    assert(zhaoxi_deliver_reminder("cloud failed reminder") == 0);
    assert(speech_calls == 2 && writes == 40 && closes == 2);
    fail_write = 1;
    assert(zhaoxi_deliver_reminder("audio failure") == -EIO);
    assert(closes == 3);
    fail_open = 1;
    assert(zhaoxi_deliver_reminder("busy audio") == -EBUSY && closes == 3);
    assert(local_scopes == 4 && local_started == 2);
    fail_write = fail_open = 0;

    /* Fill the actual outbound queue. A due one-shot must stay persisted
     * until there is room, then emit exactly one dedicated reminder. */
    for (int i = 0; i < AGENT_BUS_QUEUE_LEN; i++) {
        agent_msg_t message = {0};
        strcpy(message.channel, "cli");
        message.content = strdup("filler");
        assert(message.content && message_bus_push_outbound(&message) == 0);
    }
    assert(cron_service_start() == 0);
    add[3] = "1"; add[4] = "queue backpressure reminder";
    assert(command(5, add, "voice") == 0);
    sleep(3);
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 1);
    /* Disk must retain the pending job too. */
    FILE *file = fopen(AGENT_CRON_FILE, "r");
    assert(file); char saved[4096] = {0};
    assert(fread(saved, 1, sizeof(saved) - 1, file) > 0); fclose(file);
    assert(strstr(saved, "queue backpressure reminder"));
    for (int i = 0; i < AGENT_BUS_QUEUE_LEN; i++) {
        agent_msg_t message = {0};
        assert(message_bus_pop_outbound(&message, 0) == 0);
        assert(!strcmp(message.content, "filler"));
        message_bus_msg_free(&message);
    }
    agent_msg_t due = {0};
    assert(message_bus_pop_outbound(&due, 3000) == 0);
    assert(!strcmp(due.channel, "reminder"));
    assert(!strcmp(due.content, "queue backpressure reminder"));
    message_bus_msg_free(&due);
    cron_service_stop();
    assert(pthread_join(scheduler, NULL) == 0);
    assert(cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS) == 0);
    assert(message_bus_pop_outbound(&due, 0) != 0);
    message_bus_wakeup(); message_bus_destroy();
    puts("PASS: local reminder CRUD, time/input/save guards, offline/TTS/audio failures, real scheduler queue-full retention and single delivery.");
    return 0;
}

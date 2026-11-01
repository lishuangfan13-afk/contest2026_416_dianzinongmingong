/* SPDX-License-Identifier: Apache-2.0 */
/* Actual automatic voice worker plus actual reminder fallback. The local
 * codec double forbids capture/playback overlap, just like the BES owner. */
#define AUTO_VOICE_TESTS_MAIN baseline_auto_tests_main
#define audio_playback_open fixture_speech_playback_open
#define audio_playback_write fixture_speech_playback_write
#define audio_playback_close fixture_speech_playback_close
#include "test_auto_voice.c"
#undef audio_playback_open
#undef audio_playback_write
#undef audio_playback_close
#undef AUTO_VOICE_TESTS_MAIN

#define AGENT_CRON_MAX_JOBS 16
#define AGENT_CRON_ID_LEN 9
#define AGENT_DATA_DIR "/tmp/zhaoxi-local-alert-fixture"
#include "reminder.h"

static bool local_network;
static int local_open_failure, local_write_failure;
static int local_opens, local_closes, local_writes, local_output_seen;
static int local_active;
static atomic_int local_write_started;
static struct audio_playback local_playback;

bool network_is_connected(void) { return local_network; }
audio_playback_t *audio_playback_open(const char *path, unsigned rate,
                                     unsigned channels, unsigned bits)
{
    (void)path;
    assert(rate == 16000 && channels == 1 && bits == 16);
    assert(!s_auto.capture && atomic_load(&capture.phase) == 0);
    char status[1024];
    assert(!voice_channel_auto_status(status, sizeof(status)));
    assert(strstr(status, "\"output_active\":true"));
    assert(!local_active);
    local_opens++;
    if (local_open_failure) { errno = EBUSY; return NULL; }
    local_active = 1;
    return &local_playback;
}
int audio_playback_write(audio_playback_t *output, const void *pcm, size_t bytes)
{
    assert(output == &local_playback && pcm && bytes == 3200 && local_active);
    assert(!s_auto.capture && atomic_load(&capture.phase) == 0);
    char status[1024];
    assert(!voice_channel_auto_status(status, sizeof(status)));
    assert(strstr(status, "\"output_active\":true"));
    if (strstr(status, "\"output_playing\":true")) local_output_seen++;
    local_writes++;
    atomic_fetch_add(&local_write_started, 1);
    /* Longer than the post-output cooldown across the whole alert. A faulty
     * early resume would attempt capture before this playback has closed. */
    usleep(40000);
    assert(!s_auto.capture && atomic_load(&capture.phase) == 0);
    return local_write_failure ? -EIO : (int)bytes;
}
void audio_playback_close(audio_playback_t *output)
{
    assert(output == &local_playback && local_active);
    assert(!s_auto.capture && atomic_load(&capture.phase) == 0);
    local_active = 0;
    local_closes++;
}

/* Unused CRUD sections are discarded; these output functions are included
 * from the same product source that runs on the board. */
#include "../../../app/zhaoxi/reminder.c"

static void output_idle(void)
{
    char status[1024];
    assert(!voice_channel_auto_status(status, sizeof(status)));
    assert(strstr(status, "\"output_active\":false"));
    assert(strstr(status, "\"output_playing\":false"));
    assert(!s_auto.speakers && !local_active);
}
static void start_quiet(void)
{
    reset_fixture(); allow_bus = 1;
    assert(!voice_channel_auto_start());
    wait_count(&reads, 100);
    assert(s_auto.capture && !s_auto.awaiting);
}
static void resumed_after(int previous_reads)
{
    output_idle();
    wait_count(&reads, previous_reads + 50);
    assert(s_auto.enabled && s_auto.running && s_auto.capture);
}
static int fail_callback(void *context, void (*started)(void))
{
    assert(context == &local_playback && started && !s_auto.capture);
    started();
    return -EIO;
}
static void *parallel_alert(void *unused)
{
    (void)unused;
    return (void *)(intptr_t)zhaoxi_play_alert();
}
int main(void)
{
    assert(!voice_channel_init());
    start_quiet();
    int before = atomic_load(&reads);
    assert(!zhaoxi_deliver_reminder("offline fixture"));
    assert(local_opens == 1 && local_closes == 1 && local_writes == 20);
    assert(local_output_seen == 19);
    resumed_after(before); stop_auto();

    start_quiet(); before = atomic_load(&reads);
    local_network = true; tts_empty = 1;
    assert(!zhaoxi_deliver_reminder("failed TTS fixture"));
    assert(local_closes == 2 && local_writes == 40);
    tts_empty = 0; local_network = false;
    resumed_after(before); stop_auto();

    start_quiet(); before = atomic_load(&reads);
    local_open_failure = 1;
    assert(zhaoxi_deliver_reminder("codec open failure") == -EBUSY);
    local_open_failure = 0;
    resumed_after(before); stop_auto();

    start_quiet(); before = atomic_load(&reads);
    local_write_failure = 1;
    assert(zhaoxi_deliver_reminder("codec write failure") == -EIO);
    local_write_failure = 0;
    resumed_after(before);
    assert(voice_channel_play_local(NULL, NULL) == -EINVAL);
    assert(voice_channel_play_local(fail_callback, &local_playback) == -EIO);
    resumed_after(atomic_load(&reads)); stop_auto();

    /* An unrelated local alert cannot consume a pending question's ID. */
    reset_fixture(); allow_bus = 1; atomic_store(&fake_input_pattern, 1);
    assert(!voice_channel_auto_start()); wait_count(&fake_bus_messages, 1);
    char pending[32]; snprintf(pending, sizeof(pending), "%s", last_chat_id);
    assert(s_auto.awaiting && !s_auto.capture);
    assert(!zhaoxi_deliver_reminder("pending turn fixture"));
    assert(s_auto.awaiting && voice_channel_auto_reply_is_active(pending));
    output_idle(); before = atomic_load(&reads);
    voice_channel_auto_reply_done_for(pending);
    resumed_after(before); stop_auto();

    start_quiet(); before = atomic_load(&reads);
    pthread_t one, two;
    assert(!pthread_create(&one, NULL, parallel_alert, NULL));
    assert(!pthread_create(&two, NULL, parallel_alert, NULL));
    void *result_one, *result_two;
    assert(!pthread_join(one, &result_one) && !pthread_join(two, &result_two));
    assert(!result_one && !result_two);
    resumed_after(before); stop_auto();
    output_idle();
    /* Stopping during a bounded local alert ends capture immediately and
     * cannot be undone by the alert's later output-scope cleanup. */
    start_quiet();
    int write_before = atomic_load(&local_write_started);
    assert(!pthread_create(&one, NULL, parallel_alert, NULL));
    wait_count(&local_write_started, write_before + 1);
    assert(!voice_channel_auto_stop());
    assert(!pthread_join(auto_test_thread, NULL));
    assert(!s_auto.enabled && !s_auto.running && !s_auto.capture);
    before = atomic_load(&reads);
    assert(!pthread_join(one, &result_one) && !result_one);
    usleep(100000);
    assert(atomic_load(&reads) == before && !s_auto.enabled && !s_auto.running);
    assert(!s_auto.capture && opens == closes);
    output_idle();
    puts("PASS: actual offline/TTS-fallback reminders release capture, serialize local PCM, report playback, unwind codec/callback errors, preserve pending turns and resume automatic listening.");
    return 0;
}

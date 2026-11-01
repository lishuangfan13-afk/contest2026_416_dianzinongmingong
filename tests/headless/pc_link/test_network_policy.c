/* SPDX-License-Identifier: Apache-2.0 */
/* Actual network watcher; network and config calls are explicit doubles.
 * Real pthread scheduling checks that disconnected PC mode remains stoppable. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define OK 0
#define ERROR (-1)
#define AGENT_SKILL_SYNC_ENABLED 0
#define syslog(...) ((void)0)
static const char *TAG = "uplink-test";
static atomic_bool link_up;
static atomic_uint polls, waits, reconnects, sleeps, config_reads, service_starts;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static char configured_mode[80];
static bool missing_mode;

/* ACTUAL_SHUTDOWN_GLOBALS */

static void set_mode(const char *mode)
{
    pthread_mutex_lock(&config_lock);
    missing_mode = mode == NULL;
    snprintf(configured_mode, sizeof(configured_mode), "%s", mode ? mode : "");
    pthread_mutex_unlock(&config_lock);
}
int claw_config_get(const char *key, char *buf, size_t size)
{
    assert(!strcmp(key, "uplink_mode") && size > 0);
    atomic_fetch_add(&config_reads, 1);
    pthread_mutex_lock(&config_lock);
    int rc = missing_mode || !configured_mode[0] ? ERROR : OK;
    if (rc == OK) {
        strncpy(buf, configured_mode, size - 1);
        buf[size - 1] = '\0';
    }
    pthread_mutex_unlock(&config_lock);
    return rc;
}
bool network_is_connected(void)
{
    atomic_fetch_add(&polls, 1);
    return atomic_load(&link_up);
}
int network_wifi_reconnect(void)
{
    atomic_fetch_add(&reconnects, 1);
    atomic_store(&link_up, true);
    return OK;
}
int network_wait_connected(unsigned timeout)
{
    (void)timeout;
    atomic_fetch_add(&waits, 1);
    return network_is_connected() ? OK : ERROR;
}
const char *network_get_ip(void) { return "10.254.42.2"; }
#ifndef CONFIG_EXAMPLES_ZHAOXI
int agent_loop_start(void) { atomic_fetch_add(&service_starts, 1); return OK; }
int ws_server_start(void) { atomic_fetch_add(&service_starts, 1); return OK; }
#endif
static unsigned simulated_sleep(unsigned seconds)
{
    atomic_fetch_add(&sleeps, seconds);
    usleep(seconds * 1000);
    return 0;
}
#define sleep simulated_sleep
#ifdef CONFIG_EXAMPLES_ZHAOXI
/* ACTUAL_PRODUCT_HELPERS */
#endif
/* ACTUAL_NETWORK_WATCHER */
#undef sleep

static void reset(const char *mode, bool connected)
{
    set_mode(mode);
    atomic_store(&link_up, connected);
    atomic_store(&polls, 0); atomic_store(&waits, 0);
    atomic_store(&reconnects, 0); atomic_store(&sleeps, 0);
    atomic_store(&config_reads, 0); atomic_store(&service_starts, 0);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    agent_prepare_instance();
#else
    g_shutdown_requested = false;
#endif
}
static void await_count(atomic_uint *counter, unsigned target)
{
    for (unsigned i = 0; i < 2000 && atomic_load(counter) < target; ++i) usleep(1000);
    assert(atomic_load(counter) >= target);
}
static uint64_t monotonic_ms(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void stop(pthread_t thread)
{
    uint64_t start = monotonic_ms();
    agent_request_shutdown();
    assert(pthread_join(thread, NULL) == 0);
    assert(agent_shutdown_requested() && monotonic_ms() - start < 1000);
}
int main(void)
{
    (void)TAG;
    (void)simulated_sleep;
    pthread_t thread;
#ifdef CONFIG_EXAMPLES_ZHAOXI
    const char *pc_modes[] = {NULL, "", "pc", "unknown", "WIFI", "wifi ",
                             "wifi-with-an-unrecognized-long-suffix"};
    for (unsigned i = 0; i < sizeof(pc_modes) / sizeof(pc_modes[0]); ++i) {
        reset(pc_modes[i], false);
        assert(pthread_create(&thread, NULL, network_watch_task, NULL) == 0);
        await_count(&polls, 5);
        assert(atomic_load(&reconnects) == 0 && atomic_load(&waits) == 0);
        assert(atomic_load(&config_reads) == 1);
        /* Policy is sampled once, not reloaded in an active session. */
        set_mode("wifi");
        await_count(&polls, 9);
        assert(atomic_load(&reconnects) == 0 && atomic_load(&config_reads) == 1);
        /* PPP becomes available after Agent startup. */
        atomic_store(&link_up, true);
        await_count(&waits, 1);
        unsigned before = atomic_load(&sleeps);
        atomic_store(&link_up, false);
        await_count(&sleeps, before + 35);
        assert(atomic_load(&reconnects) == 0);
        stop(thread);
    }
    /* Existing manually connected Wi-Fi is accepted without reconfiguring it. */
    reset(NULL, true);
    assert(pthread_create(&thread, NULL, network_watch_task, NULL) == 0);
    await_count(&waits, 1);
    stop(thread);
    assert(atomic_load(&reconnects) == 0);

    /* Exact opt-in retains both initial and established-link retries. */
    reset("wifi", false);
    assert(pthread_create(&thread, NULL, network_watch_task, NULL) == 0);
    await_count(&waits, 1);
    assert(atomic_load(&reconnects) == 1 && atomic_load(&config_reads) == 1);
    set_mode("pc");
    atomic_store(&link_up, false);
    await_count(&reconnects, 2);
    assert(atomic_load(&config_reads) == 1);
    stop(thread);
    puts("PASS: actual product worker waits for late PPP, avoids automatic WAPI after loss, stops promptly; exact wifi opt-in retries and is sampled once");
#else
    reset(NULL, false);
    assert(pthread_create(&thread, NULL, network_watch_task, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&reconnects) == 1 && atomic_load(&waits) == 1);
    assert(atomic_load(&service_starts) == 2 && atomic_load(&config_reads) == 0);
    reset("pc", true);
    assert(pthread_create(&thread, NULL, network_watch_task, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&reconnects) == 0 && atomic_load(&service_starts) == 2);
    assert(atomic_load(&config_reads) == 0);
    /* Keep common helpers checked without imposing product lifetime policy. */
    assert(pthread_create(&thread, NULL, network_watch_task, NULL) == 0);
    stop(thread);
    await_count(&waits, 2);
    puts("PASS: actual standard-profile worker retains its original reconnect and service startup; product uplink mode is not consulted");
#endif
    return 0;
}

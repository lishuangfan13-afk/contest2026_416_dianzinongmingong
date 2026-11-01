/* SPDX-License-Identifier: Apache-2.0 */
/* Real SDK lifecycle, network watcher and entry wrapper. Network calls are
 * explicit doubles; a reconnect can deliberately model an uninterruptible RPC. */
#include "infra/task_lifecycle.h"
#include "cJSON.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define OK 0
#define ERROR (-1)
#define CONFIG_EXAMPLES_ZHAOXI 1
#define AGENT_SKILL_SYNC_ENABLED 0
#define syslog(...) ((void)0)
static const char *TAG="shutdown-test";
static atomic_bool link_up,watcher_ready,rpc_blocked,rpc_release;
static atomic_uint reconnect_calls;

/* ACTUAL_SHUTDOWN_GLOBALS */

bool network_is_connected(void) { return atomic_load(&link_up); }
/* Lifecycle regressions deliberately opt into Wi-Fi: the bad-order case
 * must still model a real synchronous reconnect after default PC policy. */
int claw_config_get(const char *key, char *buf, size_t size)
{
    assert(!strcmp(key,"uplink_mode"));
    assert(size>strlen("wifi"));strcpy(buf,"wifi");return OK;
}
int network_wifi_reconnect(void)
{
    atomic_fetch_add(&reconnect_calls,1);atomic_store(&rpc_blocked,true);
    while(!atomic_load(&rpc_release))usleep(1000);
    return 0;
}
int network_wait_connected(unsigned ms) { (void)ms;atomic_store(&watcher_ready,true);return network_is_connected()?0:-1; }
const char *network_get_ip(void) { return "10.254.42.2"; }
static unsigned simulated_sleep(unsigned seconds) { usleep(seconds*1000);return 0; }
#define sleep simulated_sleep
/* ACTUAL_NETWORK_PAUSE */
/* ACTUAL_NETWORK_WATCHER */
#undef sleep

static int ai_agent_run(int argc,char **argv)
{
    assert(argc==2&&!strcmp(argv[1],"--headless"));
    pthread_t watcher;
    assert(agent_tracked_pthread_create(&watcher,NULL,network_watch_task,NULL,"net_watch")==0);
    assert(pthread_detach(watcher)==0);
    while(!agent_shutdown_requested())usleep(1000);
    agent_tasks_wait(true);agent_tasks_wait(false);
    return 0;
}
/* ACTUAL_INSTANCE_ENTRY */
/* ACTUAL_SESSION_STOP */

struct launch { uint64_t owner; int result; };
static void *run_instance(void *arg)
{
    struct launch *launch=arg;char owner[32];snprintf(owner,sizeof(owner),"%" PRIu64,launch->owner);
    char *argv[]={"ai_agent","--headless","--pc-owner",owner};
    launch->result=ai_agent_main(launch->owner?4:2,argv);return NULL;
}
static void wait_flag(atomic_bool *flag)
{ for(int i=0;i<5000&&!atomic_load(flag);i++)usleep(1000);assert(atomic_load(flag)); }
static void reset_fake(void)
{
    assert(!agent_instance_is_active());
    atomic_store(&link_up,true);atomic_store(&watcher_ready,false);atomic_store(&rpc_blocked,false);
    atomic_store(&rpc_release,false);atomic_store(&reconnect_calls,0);
}
static uint64_t ms(void)
{struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
int main(void)
{
    (void)TAG;
    /* Reproduce the old teardown order: dropping PPP first lets the real
     * network watcher enter a synchronous reconnect and block real shutdown. */
    reset_fake();struct launch old={.owner=agent_session_owner_new()};assert(old.owner);
    pthread_t thread;assert(pthread_create(&thread,NULL,run_instance,&old)==0);wait_flag(&watcher_ready);
    atomic_store(&link_up,false);wait_flag(&rpc_blocked);
    cJSON *report=cJSON_CreateObject();uint64_t started=ms();
    assert(stop_session_agent(old.owner,60,report)==-ETIMEDOUT);
    assert(ms()-started<1000&&agent_instance_is_active()); /* no false release */
    assert(cJSON_IsFalse(cJSON_GetObjectItem(report,"agent_owned_instance_exited")));cJSON_Delete(report);
    atomic_store(&rpc_release,true);assert(agent_instance_wait_owned(old.owner,2000)==0);
    pthread_join(thread,NULL);assert(old.result==0);
    puts("REPRO: removing PPP before shutdown enters blocked reconnect; bounded wait reports failure and preserves live ownership");

    /* Correct order: reply has already been delivered, PPP stays UP while
     * our Agent's real network worker and instance finish, then link drops. */
    for(int i=0;i<20;i++) {
        reset_fake();struct launch owned={.owner=agent_session_owner_new()};assert(owned.owner);
        assert(pthread_create(&thread,NULL,run_instance,&owned)==0);wait_flag(&watcher_ready);
        assert(agent_instance_is_owned(owned.owner));report=cJSON_CreateObject();
        assert(stop_session_agent(owned.owner,2000,report)==0);assert(!agent_instance_is_active());
        assert(cJSON_IsTrue(cJSON_GetObjectItem(report,"agent_owned_instance_exited")));
        atomic_store(&link_up,false);pthread_join(thread,NULL);cJSON_Delete(report);
        assert(!atomic_load(&reconnect_calls)&&owned.result==0);
    }

    /* A delayed launch must not appear after its session has already closed. */
    uint64_t cancelled=agent_session_owner_new();assert(cancelled);
    assert(!agent_instance_shutdown_owned(cancelled));
    struct launch late={.owner=cancelled};run_instance(&late);assert(late.result!=0&&!agent_instance_is_active());

    /* Borrowed external instance: cancelling this session's pending token,
     * or replaying a previous token, cannot stop someone else's Agent. */
    reset_fake();struct launch external={0};assert(pthread_create(&thread,NULL,run_instance,&external)==0);wait_flag(&watcher_ready);
    uint64_t failed_owner=agent_session_owner_new();assert(failed_owner);
    struct launch rejected={.owner=failed_owner};run_instance(&rejected);assert(rejected.result!=0);
    report=cJSON_CreateObject();assert(stop_session_agent(failed_owner,2000,report)==0);
    assert(stop_session_agent(old.owner,2000,report)==0);
    assert(stop_session_agent(0,2000,report)==0&&agent_instance_is_active()&&!agent_shutdown_requested());
    cJSON_Delete(report);agent_request_shutdown();pthread_join(thread,NULL);assert(external.result==0);
    assert(agent_instance_wait_owned(0,1)==-EINVAL);
    assert(!agent_instance_is_active());
    puts("PASS: 20 real-thread owned shutdowns keep PPP up until exit; leases, stale IDs, delayed cancellation and external ownership isolation checked");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "drink.h"
#include "agent_config.h"
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static time_t clock_override;
static int fail_list_countdown;
int __real_cron_list_jobs(cron_job_t *out, int limit);
int __wrap_cron_list_jobs(cron_job_t *out, int limit)
{
    if (fail_list_countdown && !--fail_list_countdown) return -1;
    return __real_cron_list_jobs(out, limit);
}
time_t __real_time(time_t *out);
time_t __wrap_time(time_t *out)
{
    if (!clock_override) return __real_time(out);
    if (out) *out = clock_override;
    return clock_override;
}
int agent_task_create(void *(*fn)(void *), const char *name, int stack, void *arg, int priority)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; assert(0); return -1; }
int tool_registry_execute(const char *name, const char *input, char *out, size_t size)
{ (void)name; (void)input; (void)out; (void)size; assert(0); return -1; }

static int call(const char *op, const char *arg, const char *channel)
{
    char *args[] = {"zhaoxi", "drink", (char *)op, (char *)arg};
    return zhaoxi_drink_command(arg ? 4 : 3, args, 1, channel);
}
static int jobs(cron_job_t *out)
{ return cron_list_jobs(out, AGENT_CRON_MAX_JOBS); }
static int mine(cron_job_t *out)
{
    cron_job_t all[AGENT_CRON_MAX_JOBS]; int n = jobs(all), count = 0;
    for (int i = 0; i < n; i++) if (!strcmp(all[i].chat_id, "zhaoxi_drink")) {
        if (out) *out = all[i];
        count++;
    }
    return count;
}
static void *start_parallel(void *arg)
{ (void)arg; assert(call("start", "5", "cli") == 0); return NULL; }

int main(void)
{
    assert(mkdir(AGENT_DATA_DIR, 0700) == 0);
    assert(message_bus_init() == 0 && cron_service_init() == 0);
    cron_job_t unrelated = {0};
    strcpy(unrelated.name, "喝水提醒"); strcpy(unrelated.message, "user-owned reminder");
    strcpy(unrelated.channel, "cli"); strcpy(unrelated.chat_id, "zhaoxi");
    unrelated.kind = CRON_KIND_EVERY; unrelated.interval_s = 900;
    assert(cron_add_job(&unrelated) == 0);
    const char *bad[] = {"0", "-1", "+1", "1.5", "1441", "99999999999999999999999999", "", NULL};
    for (int i = 0; bad[i]; i++) assert(call("start", bad[i], "cli") != 0);
    assert(call("start", NULL, "cli") == 0);
    cron_job_t owned, all[AGENT_CRON_MAX_JOBS];
    assert(mine(&owned) == 1 && owned.interval_s == 3600 && owned.enabled);
    assert(strlen(owned.id) == 8 && jobs(all) == 2);
    assert(call("start", NULL, "cli") == 0 && mine(NULL) == 1 && jobs(all) == 2);
    assert(call("start", "5", "cli") != 0 && mine(&owned) == 1 && owned.interval_s == 3600);
    assert(call("status", NULL, "cli") == 0 && jobs(all) == 2);
    assert(call("stop", NULL, "cli") == 0 && mine(NULL) == 0 && jobs(all) == 1);
    assert(!strcmp(all[0].id, unrelated.id));
    assert(call("stop", NULL, "cli") == 0 && jobs(all) == 1);
    clock_override = 123;
    assert(call("start", "5", "cli") != 0 && mine(NULL) == 0);
    clock_override = (time_t)4102444800LL;
    assert(call("start", "5", "cli") != 0 && mine(NULL) == 0);
    clock_override = 0;
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    assert(call("start", "5", "cli") != 0 && mine(NULL) == 0 && jobs(all) == 1);
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, start_parallel, NULL));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(mine(&owned) == 1 && owned.interval_s == 300 && jobs(all) == 2);
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    assert(call("stop", NULL, "cli") != 0 && mine(NULL) == 1 && jobs(all) == 2);
    assert(call("stop", NULL, "cli") == 0 && mine(NULL) == 0 && jobs(all) == 1);
    assert(call("start", "1440", "voice") == 0 && mine(&owned) == 1);
    assert(owned.interval_s == 86400 && !strcmp(owned.channel, "voice"));
    agent_msg_t response = {0};
    assert(message_bus_pop_outbound(&response, 0) == 0);
    assert(!strcmp(response.channel, "voice") && strstr(response.content, "1440"));
    message_bus_msg_free(&response);
    assert(call("stop", NULL, "cli") == 0 && jobs(all) == 1);
    assert(!strcmp(all[0].id, unrelated.id));
    /* Core never queues a reply, even for voice. The SDK/CLI owns delivery. */
    struct zhaoxi_drink_result result;
    char *text = NULL;
    assert(!zhaoxi_drink_execute("start", 60, "voice", &result, &text));
    assert(text && strstr(text, "60") && result.verified && result.next_epoch > 0 && !result.existing && result.count == 1);
    free(text); text = NULL;
    assert(message_bus_pop_outbound(&response, 0) != 0);
    char first_id[9]; strcpy(first_id, result.id);
    assert(!zhaoxi_drink_execute("start", 60, "voice", &result, &text));
    assert(result.existing && result.verified && !strcmp(result.id, first_id));free(text);text=NULL;
    assert(!zhaoxi_drink_execute("status", 0, "voice", &result, &text));
    assert(result.enabled && result.interval_s == 3600 && result.next_epoch > 0);free(text);text=NULL;
    FILE *file=fopen(AGENT_CRON_FILE,"rb");assert(file);char saved[4096]={0};
    assert(fread(saved,1,sizeof(saved)-1,file)>0);assert(!fclose(file));
    assert(strstr(saved, first_id) && strstr(saved, "zhaoxi_drink") && strstr(saved, unrelated.id));
    assert(!zhaoxi_drink_execute("stop", 0, "voice", &result, &text));
    assert(result.removed == 1 && result.verified && result.count == 0);free(text);text=NULL;
    assert(message_bus_pop_outbound(&response, 0) != 0 && jobs(all)==1);

    /* A successful save followed by a failed readback is not confirmed or retried. */
    fail_list_countdown=2;
    assert(zhaoxi_drink_execute("start",60,"cli",&result,&text)!=0);
    assert(!result.verified && text && strstr(text,"未完成"));free(text);text=NULL;
    assert(mine(NULL)==1 && jobs(all)==2);
    fail_list_countdown=2;
    assert(zhaoxi_drink_execute("stop",0,"cli",&result,&text)!=0);
    assert(!result.verified && result.removed==1);free(text);text=NULL;
    assert(mine(NULL)==0 && jobs(all)==1 && !strcmp(all[0].id,unrelated.id));
    assert(zhaoxi_drink_execute("start",0,"voice",&result,&text)==-EINVAL && !text);
    assert(zhaoxi_drink_execute("start",60,"unknown",&result,&text)==-EINVAL && !text);
    assert(cron_remove_job(unrelated.id) == 0);
    message_bus_wakeup(); message_bus_destroy();
    puts("PASS: real persistent drink scheduling, explicit opt-in, idempotency, ownership, bounds, clock/save guards, concurrency, verified readback, outbound-free core and one CLI voice queue.");
    return 0;
}

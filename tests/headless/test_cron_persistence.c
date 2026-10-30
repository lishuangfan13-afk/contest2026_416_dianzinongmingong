/* SPDX-License-Identifier: Apache-2.0 */
#include "infra/cron_service.h"
#include "core/message_bus.h"
#include "agent_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* This test exercises the real persistence code, without a scheduler thread. */
int rand(void) { return 0; } /* Reproduce the embedded unseeded-RNG failure. */
int agent_task_create(void *(*fn)(void *), const char *name, int stack, void *arg, int prio)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)prio; assert(0); return -1; }
int tool_registry_execute(const char *name, const char *json, char *out, size_t size)
{ (void)name; (void)json; (void)out; (void)size; assert(0); return -1; }
int message_bus_push_outbound(const agent_msg_t *message)
{ (void)message; assert(0); return -1; }

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void)
{
    assert(mkdir(AGENT_DATA_DIR, 0700) == 0);
    write_text(AGENT_CRON_FILE, "{\"jobs\":[]}");
    assert(cron_service_init() == 0);
    cron_job_t first = {0};
    strcpy(first.name, "keep this reminder");
    strcpy(first.message, "saved message");
    strcpy(first.channel, "voice");
    strcpy(first.chat_id, "zhaoxi");
    first.kind = CRON_KIND_AT;
    first.at_epoch = time(NULL) + 3600;
    first.delete_after_run = true;
    assert(cron_add_job(&first) == 0 && first.id[0]);
    cron_job_t jobs[16];
    assert(cron_list_jobs(jobs, 16) == 1);
    assert(cron_service_init() == 0 && cron_list_jobs(jobs, 16) == 1);
    assert(!strcmp(first.id, jobs[0].id));

    /* /dev/full accepts open and buffered fwrite, then fails at flush. */
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    cron_job_t second = first;
    strcpy(second.name, "must not be confirmed");
    assert(cron_add_job(&second) != 0 && !second.id[0]);
    assert(cron_list_jobs(jobs, 16) == 1);
    assert(!strcmp(jobs[0].id, first.id));
    assert(symlink("/dev/full", AGENT_CRON_FILE ".tmp") == 0);
    assert(cron_remove_job(first.id) != 0);
    assert(cron_list_jobs(jobs, 16) == 1 && !strcmp(jobs[0].id, first.id));
    assert(cron_service_init() == 0 && cron_list_jobs(jobs, 16) == 1);

    /* Simulate power loss after the old file moved to its recovery name. */
    unlink(AGENT_CRON_FILE ".bak");
    assert(rename(AGENT_CRON_FILE, AGENT_CRON_FILE ".bak") == 0);
    assert(cron_service_init() == 0 && cron_list_jobs(jobs, 16) == 1);
    assert(cron_remove_job(first.id) == 0 && cron_list_jobs(jobs, 16) == 0);
    assert(cron_service_init() == 0 && cron_list_jobs(jobs, 16) == 0);
    cron_job_t a = first, b = first;
    assert(cron_add_job(&a) == 0 && cron_add_job(&b) == 0);
    assert(strcmp(a.id, b.id) != 0);
    assert(cron_remove_job(a.id) == 0);
    assert(cron_list_jobs(jobs, 16) == 1 && !strcmp(jobs[0].id, b.id));
    assert(cron_remove_job(b.id) == 0);
    write_text(AGENT_CRON_FILE, "not valid JSON");
    assert(cron_service_init() != 0);
    puts("PASS: persistent add/remove, reload, flush-failure rollback, recovery copy, corrupt-data refusal and unique IDs with rand() always zero.");
    return 0;
}

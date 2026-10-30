/* SPDX-License-Identifier: Apache-2.0 */
#include "agent_config.h"
#include "tools/skill_loader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Cache invalidation is the only test double; file discovery is real. */
static int invalidations;
void tool_registry_invalidate(void) { invalidations++; }

int main(void)
{
    assert(mkdir(AGENT_DATA_DIR, 0700) == 0);
    assert(mkdir(AGENT_SKILLS_DIR, 0700) == 0);
    FILE *old = fopen(AGENT_SKILLS_DIR "health-reminder.md", "w");
    assert(old);
    fputs("# Legacy Health\nOld extension\n", old);
    fclose(old);
    assert(skill_loader_init() == 0);
    assert(access(AGENT_SKILLS_DIR "reminder.md", F_OK) == 0);
    assert(access(AGENT_SKILLS_DIR "note-taker.md", F_OK) == 0);
    char summary[16384];
    size_t length = skill_loader_build_summary(summary, sizeof(summary));
    assert(length > 0 && length == strlen(summary));
    assert(strstr(summary, "note-taker.md") && strstr(summary, "reminder.md"));
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert(access(AGENT_SKILLS_DIR "task-manager.md", F_OK) != 0);
    assert(access(AGENT_SKILLS_DIR "daily-briefing.md", F_OK) != 0);
    assert(!strstr(summary, "health-reminder.md"));
    assert(!strstr(summary, "weather.md"));
#else
    assert(strstr(summary, "task-manager.md"));
    assert(strstr(summary, "weather.md"));
    assert(strstr(summary, "health-reminder.md"));
#endif
    /* Existing deployed core assets must survive built-in installation. */
    FILE *core = fopen(AGENT_SKILLS_DIR "note-taker.md", "w");
    assert(core);
    fputs("# 智能记事本\n记录备忘与稳定编号待办。\n", core);
    fclose(core);
    assert(skill_loader_init() == 0);
    skill_loader_build_summary(summary, sizeof(summary));
    assert(strstr(summary, "智能记事本"));
    char tiny[17];
    memset(tiny, '!', sizeof(tiny));
    assert(skill_loader_build_summary(tiny, 16) <= 15);
    assert(tiny[15] == 0 && tiny[16] == '!');
    assert(skill_loader_build_summary(NULL, 0) == 0);
    skill_loader_refresh();
    assert(invalidations == 1);
    return 0;
}

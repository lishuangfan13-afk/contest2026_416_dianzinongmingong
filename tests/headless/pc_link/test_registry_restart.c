/* SPDX-License-Identifier: Apache-2.0 */
/* The registry and cJSON under test are the actual SDK sources. Tool bodies
 * and the security guard are test doubles; no network or file IO occurs. */
#include "tools/tool_registry.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_tools(int expected)
{
    static const char *names[] = {"get_current_time", "read_file", "write_file",
        "edit_file", "list_dir", "cron_add", "cron_list", "cron_remove"};
    char *text = tool_registry_get_tools_json();
    assert(text != NULL);
    cJSON *tools = cJSON_Parse(text);
    assert(cJSON_IsArray(tools));
    assert(cJSON_GetArraySize(tools) == expected);
    for (int i = 0; i < expected; i++) {
        cJSON *tool = cJSON_GetArrayItem(tools, i);
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
        assert(name && !strcmp(name, names[i]));
        assert(cJSON_IsObject(cJSON_GetObjectItem(tool, "input_schema")));
    }
    cJSON_Delete(tools);
    free(text);
}

int main(void)
{
    for (int cycle = 0; cycle < 20; cycle++) {
        assert(tool_registry_init() == 0);
        expect_tools(8);
        /* Re-init is also required to replace a previously built empty cache. */
        assert(tool_registry_init() == 0);
        expect_tools(8);
        tool_registry_cleanup();
        expect_tools(0);
    }
    tool_registry_cleanup();
    puts("registry restart: 20 lifetimes preserve all 8 tool schemas; empty cache recovers");
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZHAOXI_TODO_STORE_H
#define ZHAOXI_TODO_STORE_H

struct zhaoxi_todo_result {
    unsigned id;
    unsigned count; /* Number of unfinished entries. */
    int changed;
};

/* Read/write the shared Markdown file directly. On success, caller owns reply.
 * Existing bytes are preserved except the selected completion checkbox. */
int zhaoxi_todo_execute(const char *path, const char *operation, const char *argument,
                        struct zhaoxi_todo_result *result, char **reply);
#endif

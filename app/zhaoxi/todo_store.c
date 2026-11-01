/* SPDX-License-Identifier: Apache-2.0 */
#include "todo_store.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TODO_FILE_LIMIT (64 * 1024)
#define TODO_ID_LIMIT 9999
static pthread_mutex_t todo_lock = PTHREAD_MUTEX_INITIALIZER;

static int failure(void) { return -(errno ? errno : EIO); }

static int number(const char *text, const char *end, unsigned *value)
{
    unsigned n = 0;
    if (text == end) return -EINVAL;
    for (; text < end; text++) {
        if (*text < '0' || *text > '9') return -EINVAL;
        n = n * 10 + (unsigned)(*text - '0');
        if (n > TODO_ID_LIMIT) return -ERANGE;
    }
    if (!n) return -EINVAL;
    *value = n;
    return 0;
}

struct parsed {
    unsigned maximum;
    unsigned count;
    char *target;
};

static int inspect(char *data, unsigned target, struct parsed *state, char *listing)
{
    uint8_t seen[(TODO_ID_LIMIT + 8) / 8] = {0};
    size_t listed = 0;
    memset(state, 0, sizeof(*state));
    for (char *line = data; *line;) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        char *p = line;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (end - p >= 3 && (p[0] == '-' || p[0] == '*') && p[1] == ' ' && p[2] == '[') {
            if (end - p < 7 || p[4] != ']' || p[5] != ' ' ||
                (p[3] != ' ' && p[3] != 'x' && p[3] != 'X')) return -EINVAL;
            char *id_start = p + 6;
            while (id_start < end && *id_start == ' ') id_start++;
            if (id_start == end || *id_start++ != '[') return -EINVAL;
            char *id_end = memchr(id_start, ']', (size_t)(end - id_start));
            unsigned id;
            if (!id_end || number(id_start, id_end, &id)) return -EINVAL;
            if (id_end + 1 >= end || id_end[1] != ' ') return -EINVAL;
            char *body = id_end + 2;
            char *body_end = end;
            if (body_end > body && body_end[-1] == '\r') body_end--;
            if (body >= body_end || (seen[id / 8] & (1u << (id % 8)))) return -EINVAL;
            seen[id / 8] |= (uint8_t)(1u << (id % 8));
            if (id > state->maximum) state->maximum = id;
            if (id == target) state->target = p + 3;
            if (p[3] == ' ') {
                state->count++;
                if (listing) {
                    /* Every displayed task comes from this validated file. */
                    size_t length = (size_t)(body_end - body);
                    int n = snprintf(listing + listed, TODO_FILE_LIMIT + 128 - listed,
                                     "[%u] %.*s\n", id, (int)length, body);
                    if (n < 0 || (size_t)n >= TODO_FILE_LIMIT + 128 - listed) return -E2BIG;
                    listed += (size_t)n;
                }
            }
        }
        line = *end ? end + 1 : end;
    }
    return 0;
}

static int save(const char *path, const char *data, size_t length)
{
    char *temporary = malloc(strlen(path) + 5);
    if (!temporary) return -ENOMEM;
    sprintf(temporary, "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    int rc = file ? 0 : failure();
    if (file) {
        if (fwrite(data, 1, length, file) != length || fflush(file)) rc = failure();
        if (!rc && fsync(fileno(file))) rc = failure();
        if (fclose(file) && !rc) rc = failure();
        if (!rc && rename(temporary, path)) rc = failure();
    }
    if (rc) unlink(temporary);
    free(temporary);
    return rc;
}

int zhaoxi_todo_execute(const char *path, const char *operation, const char *argument,
                        struct zhaoxi_todo_result *result, char **reply)
{
    if (!path || !operation || !result || !reply) return -EINVAL;
    memset(result, 0, sizeof(*result)); *reply = NULL;
    int add = !strcmp(operation, "add"), list = !strcmp(operation, "list");
    int done = !strcmp(operation, "done");
    unsigned target = 0;
    if ((!add && !list && !done) || (!list && (!argument || !*argument))) return -EINVAL;
    if (done && number(argument, argument + strlen(argument), &target)) return -EINVAL;
    if (add) {
        if (strlen(argument) > 511) return -E2BIG;
        for (const unsigned char *p = (const unsigned char *)argument; *p; p++)
            if (*p < 32 || *p == 127) return -EINVAL;
    }
    int locked = pthread_mutex_lock(&todo_lock);
    if (locked) return -locked;
    char *data = malloc(TODO_FILE_LIMIT + 2), *answer = calloc(1, TODO_FILE_LIMIT + 128);
    int rc = data && answer ? 0 : -ENOMEM;
    size_t length = 0;
    if (!rc) {
        FILE *file = fopen(path, "rb");
        if (!file) { if (errno != ENOENT) rc = failure(); }
        else {
            length = fread(data, 1, TODO_FILE_LIMIT + 1, file);
            if (ferror(file)) rc = failure();
            if (fclose(file) && !rc) rc = failure();
            if (length > TODO_FILE_LIMIT) rc = -EFBIG;
            if (!rc && memchr(data, 0, length)) rc = -EINVAL;
        }
        data[length <= TODO_FILE_LIMIT ? length : 0] = 0;
    }
    struct parsed state = {0};
    if (!rc) rc = inspect(data, target, &state, list ? answer : NULL);
    if (!rc) {
        result->count = state.count;
        if (add) {
            if (state.maximum == TODO_ID_LIMIT) rc = -ENOSPC;
            else {
                result->id = state.maximum + 1;
                size_t extra = strlen(argument) + 40;
                if (length + extra > TODO_FILE_LIMIT) rc = -EFBIG;
                else {
                    char *updated = malloc(length + extra);
                    if (!updated) rc = -ENOMEM;
                    else {
                        memcpy(updated, data, length);
                        int n = snprintf(updated + length, extra, "%s- [ ] [%u] %s\n",
                                         length && data[length - 1] != '\n' ? "\n" : "",
                                         result->id, argument);
                        if (n < 0 || (size_t)n >= extra) rc = -E2BIG;
                        else rc = save(path, updated, length + (size_t)n);
                        free(updated);
                    }
                    if (!rc) snprintf(answer, TODO_FILE_LIMIT + 128,
                                      "已添加待办 [%u]：%s", result->id, argument);
                }
            }
        } else if (done) {
            result->id = target;
            if (!state.target) rc = -ENOENT;
            else if (*state.target == ' ') {
                *state.target = 'x';
                rc = save(path, data, length);
                if (!rc) result->changed = 1;
            }
            if (!rc) snprintf(answer, TODO_FILE_LIMIT + 128, result->changed ?
                              "待办 [%u] 已完成并保存。" : "待办 [%u] 已是完成状态。", target);
        } else if (!state.count) strcpy(answer, "当前没有未完成待办。");
    }
    free(data);
    if (!rc) *reply = answer;
    else free(answer);
    pthread_mutex_unlock(&todo_lock);
    return rc;
}

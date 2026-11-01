/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZHAOXI_JOURNAL_H
#define ZHAOXI_JOURNAL_H
#include <stddef.h>
#include <time.h>
int zhaoxi_note_path(time_t utc, char *path, size_t capacity);
struct zhaoxi_note_result {
    size_t bytes;
    int verified;
    int storage_uncertain;
};
/* Returned reply is heap-allocated and owned by the caller on success. */
int zhaoxi_note_execute(time_t utc, const char *operation, const char *argument,
                       struct zhaoxi_note_result *result, char **reply);
#endif

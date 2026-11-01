/* SPDX-License-Identifier: Apache-2.0 */
#include "journal.h"
#include "agent_config.h"
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NOTE_FILE_LIMIT (64 * 1024)
#define NOTE_TEXT_LIMIT 511
static pthread_mutex_t note_lock = PTHREAD_MUTEX_INITIALIZER;

/* Business dates follow the same Beijing calendar as get_current_time and
 * memory_store, while the device clock and reminder epochs remain UTC. */
int zhaoxi_note_path(time_t utc, char *path, size_t capacity)
{
    if (!path || !capacity || (int64_t)utc < 1735689600LL || (int64_t)utc > 4102444800LL)
        return -EINVAL;
    int64_t shifted = (int64_t)utc + 8 * 3600;
    time_t display = (time_t)shifted;
    if ((int64_t)display != shifted) return -EOVERFLOW;
    struct tm today;
    if (!gmtime_r(&display, &today)) return -EINVAL;
    if (!strftime(path, capacity, AGENT_DATA_DIR "/memory/daily/%Y-%m-%d.md", &today))
        return -ENOSPC;
    return 0;
}

static int io_failure(void) { return -(errno ? errno : EIO); }

static int make_parent(const char *path)
{
    char directory[256];
    if (strlen(path) >= sizeof(directory)) return -ENAMETOOLONG;
    strcpy(directory, path);
    for (char *p = directory + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(directory, 0700) && errno != EEXIST) return io_failure();
        struct stat state;
        if (stat(directory, &state)) return io_failure();
        if (!S_ISDIR(state.st_mode)) return -ENOTDIR;
        *p = '/';
    }
    return 0;
}

static int read_all(int fd, char *data, size_t *length)
{
    if (lseek(fd, 0, SEEK_SET) < 0) return io_failure();
    size_t used = 0;
    while (used < NOTE_FILE_LIMIT + 1) {
        ssize_t n = read(fd, data + used, NOTE_FILE_LIMIT + 1 - used);
        if (n < 0) { if (errno == EINTR) continue; return io_failure(); }
        if (!n) break;
        used += (size_t)n;
    }
    if (used > NOTE_FILE_LIMIT) return -EFBIG;
    if (memchr(data, 0, used)) return -EINVAL;
    data[used] = '\0';
    *length = used;
    return 0;
}

int zhaoxi_note_execute(time_t utc, const char *operation, const char *argument,
                       struct zhaoxi_note_result *result, char **reply)
{
    if (!operation || !result || !reply) return -EINVAL;
    memset(result, 0, sizeof(*result)); *reply = NULL;
    int add = !strcmp(operation, "add"), list = !strcmp(operation, "list");
    if (!add && !list) return -EINVAL;
    if (add) {
        if (!argument || !*argument) return -EINVAL;
        size_t length = strlen(argument);
        if (length > NOTE_TEXT_LIMIT) return -E2BIG;
        for (size_t i = 0; i < length; i++)
            if ((unsigned char)argument[i] < 32 || (unsigned char)argument[i] == 127) return -EINVAL;
    }
    char path[256];
    int rc = zhaoxi_note_path(utc, path, sizeof(path));
    if (rc) return rc;
    /* Reject overlapping local requests promptly instead of queuing an
     * unbounded number of note operations behind storage I/O. */
    int locked = pthread_mutex_trylock(&note_lock);
    if (locked) return -locked;
    char *before = malloc(NOTE_FILE_LIMIT + 2), *after = malloc(NOTE_FILE_LIMIT + 2);
    char *answer = malloc(NOTE_FILE_LIMIT + 128);
    rc = before && after && answer ? 0 : -ENOMEM;
    int fd = -1;
    size_t length = 0;
    if (!rc && add) rc = make_parent(path);
    if (!rc) {
        fd = open(path, add ? O_RDWR | O_CREAT | O_APPEND : O_RDONLY, 0600);
        if (fd < 0) {
            if (!list || errno != ENOENT) rc = io_failure();
            else before[0] = '\0';
        } else rc = read_all(fd, before, &length);
    }
    if (!rc && add) {
        char record[NOTE_TEXT_LIMIT + 5];
        int n = snprintf(record, sizeof(record), "%s- %s\n",
                         length && before[length - 1] != '\n' ? "\n" : "", argument);
        if (n < 0 || (size_t)n >= sizeof(record)) rc = -E2BIG;
        else if (length + (size_t)n > NOTE_FILE_LIMIT) rc = -EFBIG;
        else {
            size_t sent = 0;
            while (sent < (size_t)n) {
                ssize_t wrote = write(fd, record + sent, (size_t)n - sent);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) { rc = io_failure(); break; }
                result->storage_uncertain = 1;
                sent += (size_t)wrote;
            }
            if (!rc && fsync(fd)) rc = io_failure();
            size_t checked = 0;
            if (!rc) rc = read_all(fd, after, &checked);
            if (!rc && (checked != length + (size_t)n || memcmp(before, after, length) ||
                        memcmp(after + length, record, (size_t)n))) rc = -EIO;
            if (!rc) {
                /* Detect a path replacement during our append/readback. */
                struct stat opened, current;
                if (fstat(fd, &opened) || stat(path, &current)) rc = io_failure();
                else if (opened.st_dev != current.st_dev || opened.st_ino != current.st_ino ||
                         opened.st_size != current.st_size) rc = -EIO;
            }
            if (!rc) {
                result->bytes = checked;
                result->verified = 1;
                snprintf(answer, NOTE_FILE_LIMIT + 128, "已记录并读回确认：%s", argument);
            }
        }
    } else if (!rc) {
        result->bytes = length;
        result->verified = 1;
        if (length) snprintf(answer, NOTE_FILE_LIMIT + 128, "今天的备忘：\n%s", before);
        else strcpy(answer, "今天还没有备忘。");
    }
    if (fd >= 0 && close(fd) && !rc) rc = io_failure();
    if (!rc) { result->storage_uncertain = 0; *reply = answer; }
    else { result->verified = 0; free(answer); }
    free(before); free(after);
    pthread_mutex_unlock(&note_lock);
    return rc;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include "console_lease.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CLAIM_MS 250
#define RETRY_NS 1000000L

/* Pending is published BEFORE waiting for an in-flight reader: new readers
 * cannot starve claim by repeatedly reacquiring the gate. No mutex_lock or
 * timedlock (whose deadline may use adjustable wall time) is used here.
 * Ownership metadata is atomic as foreign release attempts may inspect it
 * while another thread is still claiming. pthread_t is scalar on NuttX.
 */
static atomic_flag gate = ATOMIC_FLAG_INIT;
static atomic_bool pending = ATOMIC_VAR_INIT(false);
static _Atomic(pthread_t) owner;
static atomic_bool owned = ATOMIC_VAR_INIT(false);

static bool console_fd(int fd)
{
    struct stat input, console;
    int saved = errno;
    bool match = false;
#ifdef FIOC_FILEPATH
    char path[PATH_MAX];
    if (ioctl(fd, FIOC_FILEPATH, (unsigned long)path) == 0 &&
        strcmp(path, "/dev/console") == 0) {
        match = true;
        goto done;
    }
#endif
    /* isatty alone would capture every UART/PTY. NuttX pseudo-inodes may
     * supply zero IDs: never accept two all-zero stat records as identity.
     * On POSIX, st_rdev identifies aliases of the same character device.
     */
    if (fstat(fd, &input) == 0 && stat("/dev/console", &console) == 0 &&
        S_ISCHR(input.st_mode) && S_ISCHR(console.st_mode)) {
        match = (input.st_ino != 0 && console.st_ino != 0 &&
                 input.st_dev == console.st_dev &&
                 input.st_ino == console.st_ino) ||
                (input.st_rdev != 0 && input.st_rdev == console.st_rdev);
    }
#ifdef FIOC_FILEPATH
done:
#endif
    errno = saved;
    return match;
}

int zhaoxi_console_readline_begin(int fd)
{
    if (!console_fd(fd)) return 0;
    if (atomic_load(&pending)) return -EAGAIN;
    if (atomic_flag_test_and_set(&gate)) return -EAGAIN;
    /* A claimant may publish pending between the first check and the gate. */
    if (atomic_load(&pending)) {
        atomic_flag_clear(&gate);
        return -EAGAIN;
    }
    return 1;
}

void zhaoxi_console_readline_end(void)
{
    atomic_flag_clear(&gate);
}

static int monotonic_ms(int64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -errno;
    *out = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

int zhaoxi_console_claim(void)
{
    bool expected = false;
    int64_t start, now;
    int rc;
    if (!atomic_compare_exchange_strong(&pending, &expected, true))
        return -EBUSY;
    rc = monotonic_ms(&start);
    if (rc) goto fail;
    for (;;) {
        rc = monotonic_ms(&now);
        if (rc) goto fail;
        if (now - start >= CLAIM_MS) { rc = -ETIMEDOUT; goto fail; }
        if (!atomic_flag_test_and_set(&gate)) {
            atomic_store(&owner, pthread_self());
            atomic_store(&owned, true);
            return 0;
        }
        struct timespec pause = {0, RETRY_NS};
        if (nanosleep(&pause, NULL) < 0 && errno != EINTR) {
            rc = -errno;
            goto fail;
        }
    }
fail:
    atomic_store(&pending, false);
    return rc;
}

int zhaoxi_console_release(void)
{
    /* pending stays true until gate has been released. Thus readers cannot
     * sneak in between restoring TTY and publishing the completed handoff.
     * A foreign release cannot clear either a pending or an owned claim.
     */
    if (!atomic_load(&owned) ||
        !pthread_equal(atomic_load(&owner), pthread_self())) return -EPERM;
    atomic_store(&owned, false);
    atomic_flag_clear(&gate);
    atomic_store(&pending, false);
    return 0;
}

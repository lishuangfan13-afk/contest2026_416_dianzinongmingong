/* SPDX-License-Identifier: Apache-2.0 */
/* Actual product C + patched readline_fd.c, only OS I/O/time are doubles.
 * No device, PTY, socket, real serial port or network is opened. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <nuttx/config.h>
#include "readline.h"
#ifdef TEST_NO_ECHO
#undef CONFIG_READLINE_ECHO
#endif

#define CONSOLE 40
#define OTHER_TTY 41
#define ALIAS 42
#define ZERO_IDS 43
#define FIOC_FILEPATH 0x5a01
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_event = PTHREAD_COND_INITIALIZER;
static struct {
    unsigned polls, reads, writes, termios_sets, lease_sleeps;
    unsigned poll_enters, read_enters;
    int poll_error, read_error, sleep_error, clock_error;
    bool eof, path_failure, stat_failure, zero_console, fake_time, freeze_read;
    int64_t now;
    char queue[32];
    size_t length;
    struct termios tty;
} io;

static struct timespec deadline_ms(unsigned ms)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return ts;
}
static void reset_io(void)
{
    pthread_mutex_lock(&io_lock);
    memset(&io, 0, sizeof(io));
    io.tty.c_lflag = ICANON | ECHO;
    pthread_mutex_unlock(&io_lock);
}
static void enqueue(char byte)
{
    pthread_mutex_lock(&io_lock);
    assert(io.length < sizeof(io.queue));
    io.queue[io.length++] = byte;
    pthread_cond_broadcast(&io_event);
    pthread_mutex_unlock(&io_lock);
}
static void wait_counter(unsigned *counter, unsigned target)
{
    struct timespec until = deadline_ms(2000);
    pthread_mutex_lock(&io_lock);
    while (*counter < target) assert(pthread_cond_timedwait(&io_event, &io_lock, &until) == 0);
    pthread_mutex_unlock(&io_lock);
}
static int fake_fstat(int fd, struct stat *out)
{
    memset(out, 0, sizeof(*out));
    if (io.stat_failure) { errno = EBADF; return -1; }
    out->st_mode = S_IFCHR;
    if (fd != ZERO_IDS) {
        out->st_dev = 3;
        out->st_ino = fd == OTHER_TTY ? 8 : 7;
        out->st_rdev = fd == OTHER_TTY ? 12 : 11;
    }
    return 0;
}
static int fake_stat(const char *path, struct stat *out)
{
    assert(strcmp(path, "/dev/console") == 0);
    int rc = fake_fstat(CONSOLE, out);
    if (io.zero_console) out->st_dev = out->st_ino = out->st_rdev = 0;
    return rc;
}
static int fake_ioctl(int fd, unsigned long request, unsigned long arg)
{
    assert(request == FIOC_FILEPATH);
    if (io.path_failure) { errno = ENOTTY; return -1; }
    strcpy((char *)arg, fd == CONSOLE ? "/dev/console" : "/dev/ttyS1");
    return 0;
}
static int fake_clock_gettime(clockid_t clock, struct timespec *out)
{
    assert(clock == CLOCK_MONOTONIC);
    if (io.clock_error) { errno = io.clock_error; return -1; }
    if (!io.fake_time) return clock_gettime(clock, out);
    out->tv_sec = io.now / 1000;
    out->tv_nsec = io.now % 1000 * 1000000L;
    return 0;
}
static int fake_nanosleep(const struct timespec *req, struct timespec *remaining)
{
    (void)remaining;
    assert(req->tv_sec == 0 && (req->tv_nsec == 1000000L || req->tv_nsec == 100000000L));
    pthread_mutex_lock(&io_lock);
    if (io.sleep_error) {
        int err = io.sleep_error;
        io.sleep_error = 0;
        if (io.fake_time) io.now++;
        pthread_mutex_unlock(&io_lock);
        errno = err;
        return -1;
    }
    if (req->tv_nsec == 100000000L) {
        io.lease_sleeps++;
        pthread_cond_broadcast(&io_event);
    }
    if (io.fake_time) {
        io.now += req->tv_nsec / 1000000L;
        pthread_mutex_unlock(&io_lock);
        return 0;
    }
    pthread_mutex_unlock(&io_lock);
    return nanosleep(req, NULL);
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1);
    assert(fds->fd == CONSOLE || fds->fd == ALIAS);
    pthread_mutex_lock(&io_lock);
    io.polls++;
    if (fds->events == POLLOUT) {
        assert(timeout == 0);
        fds->revents = POLLOUT;
        pthread_mutex_unlock(&io_lock);
        return 1;
    }
    assert(fds->events == POLLIN && timeout == 50);
    io.poll_enters++;
    pthread_cond_broadcast(&io_event);
    if (io.poll_error) {
        int err = io.poll_error;
        io.poll_error = 0;
        pthread_mutex_unlock(&io_lock);
        errno = err;
        return -1;
    }
    struct timespec until = deadline_ms((unsigned)timeout);
    while (!io.length && !io.eof) {
        int rc = pthread_cond_timedwait(&io_event, &io_lock, &until);
        if (rc == ETIMEDOUT) break;
        assert(rc == 0);
    }
    fds->revents = io.length ? POLLIN : io.eof ? POLLHUP : 0;
    int result = fds->revents != 0;
    pthread_mutex_unlock(&io_lock);
    return result;
}
static ssize_t fake_read(int fd, void *out, size_t count)
{
    assert(fd >= CONSOLE && fd <= ZERO_IDS && count == 1);
    pthread_mutex_lock(&io_lock);
    io.reads++;
    io.read_enters++;
    pthread_cond_broadcast(&io_event);
    while (io.freeze_read) pthread_cond_wait(&io_event, &io_lock);
    if (io.read_error) {
        int err = io.read_error;
        io.read_error = 0;
        pthread_mutex_unlock(&io_lock);
        errno = err;
        return -1;
    }
    if (io.length) {
        *(char *)out = io.queue[0];
        memmove(io.queue, io.queue + 1, --io.length);
        pthread_mutex_unlock(&io_lock);
        return 1;
    }
    assert(io.eof);
    pthread_mutex_unlock(&io_lock);
    return 0;
}
static ssize_t fake_write(int fd, const void *data, size_t size)
{
    (void)data;
    assert(fd == CONSOLE || fd == OTHER_TTY);
    pthread_mutex_lock(&io_lock);
    io.writes += size;
    pthread_mutex_unlock(&io_lock);
    return (ssize_t)size;
}
static int fake_isatty(int fd) { return fd != ZERO_IDS; }
static int fake_tcgetattr(int fd, struct termios *out)
{
    (void)fd;
    pthread_mutex_lock(&io_lock);
    *out = io.tty;
    pthread_mutex_unlock(&io_lock);
    return 0;
}
static int fake_tcsetattr(int fd, int action, const struct termios *cfg)
{
    (void)fd;
    assert(action == TCSANOW);
    pthread_mutex_lock(&io_lock);
    io.tty = *cfg;
    io.termios_sets++;
    pthread_mutex_unlock(&io_lock);
    return 0;
}

/* Rename the real implementations in absence/partial-hook configurations so
 * the actual weak symbols remain unresolved exactly as on non-product apps. */
#ifdef NO_PRODUCT_HOOK
#define zhaoxi_console_readline_begin disabled_begin
#endif
#if defined(NO_PRODUCT_HOOK) || defined(PARTIAL_PRODUCT_HOOK)
#define zhaoxi_console_readline_end disabled_end
#endif
#define fstat fake_fstat
#define stat(path, out) fake_stat(path, out)
#define ioctl fake_ioctl
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
/* ACTUAL_CONSOLE_LEASE */
#undef fstat
#undef stat
#undef ioctl
#undef clock_gettime
#undef zhaoxi_console_readline_begin
#undef zhaoxi_console_readline_end

#define poll fake_poll
#define read fake_read
#define write fake_write
#define isatty fake_isatty
#define tcgetattr fake_tcgetattr
#define tcsetattr fake_tcsetattr
/* ACTUAL_READLINE_FD */
#undef read
#undef write
#undef poll
#undef nanosleep
#undef isatty
#undef tcgetattr
#undef tcsetattr

/* Common editor adapter drives the real fd getc/echo callbacks. The test
 * targets fd ownership, leaving command editing out of scope. */
ssize_t readline_common(struct rl_common_s *vtbl, char *buf, int size)
{
    assert(size >= 2);
    int ch = vtbl->rl_getc(vtbl);
    if (ch == EOF) return EOF;
    buf[0] = (char)ch;
    buf[1] = 0;
#ifdef CONFIG_READLINE_ECHO
    vtbl->rl_putc(vtbl, ch);
#endif
    return 1;
}
static struct readline_s fd_vtbl(int fd)
{
    struct readline_s vtbl = {.infd = fd};
#ifdef CONFIG_READLINE_ECHO
    vtbl.outfd = fd;
#endif
    return vtbl;
}

#if !defined(NO_PRODUCT_HOOK) && !defined(PARTIAL_PRODUCT_HOOK)
static void assert_gate_free(void)
{
    assert(zhaoxi_console_readline_begin(CONSOLE) == 1);
    zhaoxi_console_readline_end();
    assert(zhaoxi_console_claim() == 0);
    assert(zhaoxi_console_release() == 0);
}
static void *reader_thread(void *result)
{
    char text[8];
    *(ssize_t *)result = readline_fd(text, sizeof(text), CONSOLE, CONSOLE);
    assert(text[0] == 's');
    return NULL;
}
static void *foreign_release(void *unused)
{
    (void)unused;
    assert(zhaoxi_console_release() == -EPERM);
    assert(zhaoxi_console_claim() == -EBUSY);
    return NULL;
}
static void idle_handoff(void)
{
    reset_io();
    pthread_t reader, foreign;
    ssize_t result = 0;
    assert(pthread_create(&reader, NULL, reader_thread, &result) == 0);
    wait_counter(&io.poll_enters, 1);
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(zhaoxi_console_claim() == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    int64_t elapsed = (after.tv_sec - before.tv_sec) * 1000 +
                      (after.tv_nsec - before.tv_nsec) / 1000000;
    assert(elapsed < 250);
    assert(zhaoxi_console_claim() == -EBUSY);
    assert(pthread_create(&foreign, NULL, foreign_release, NULL) == 0);
    assert(pthread_join(foreign, NULL) == 0);
    /* PPP's raw mode is represented by this termios value, and its byte
     * remains in the simulated device until PPP itself drains it. */
    pthread_mutex_lock(&io_lock);
    io.tty.c_lflag = 0;
    unsigned sets = io.termios_sets, polls = io.polls;
    pthread_mutex_unlock(&io_lock);
    enqueue('~');
    wait_counter(&io.lease_sleeps, 2);
    struct readline_s delayed = fd_vtbl(CONSOLE);
#ifdef CONFIG_READLINE_ECHO
    readline_putc(&delayed.vtbl, 'x');
    readline_write(&delayed.vtbl, "late", 4);
#else
    (void)delayed;
#endif
    pthread_mutex_lock(&io_lock);
    assert(io.reads == 0 && io.writes == 0 && io.length == 1);
    assert(io.polls == polls && io.termios_sets == sets && io.tty.c_lflag == 0);
    io.length = 0; /* PPP, the exclusive owner, consumes '~'. */
    io.tty.c_lflag = 0; /* Caller restored readline's pre-PPP mode. */
    pthread_mutex_unlock(&io_lock);
    assert(zhaoxi_console_release() == 0);
    enqueue('s');
    assert(pthread_join(reader, NULL) == 0 && result == 1);
    assert(io.reads == 1);
#ifdef CONFIG_READLINE_ECHO
    assert(io.writes == 1);
#else
    assert(io.writes == 0);
#endif
    assert(io.tty.c_lflag == (ICANON | ECHO));
    assert(zhaoxi_console_release() == -EPERM);
    assert_gate_free();
}
static void identity_and_errors(void)
{
    reset_io();
    errno = EDOM;
    assert(zhaoxi_console_readline_begin(OTHER_TTY) == 0 && errno == EDOM);
    assert(zhaoxi_console_readline_begin(ALIAS) == 1);
    zhaoxi_console_readline_end();
    io.path_failure = true;
    assert(zhaoxi_console_readline_begin(CONSOLE) == 1);
    zhaoxi_console_readline_end();
    io.zero_console = true;
    assert(zhaoxi_console_readline_begin(ZERO_IDS) == 0);
    io.stat_failure = true;
    assert(zhaoxi_console_readline_begin(CONSOLE) == 0);
    reset_io();
    struct readline_s v = fd_vtbl(CONSOLE);
    enqueue('a'); io.poll_error = EINTR; io.read_error = EINTR;
    assert(readline_getc(&v.vtbl) == 'a');
    assert_gate_free();
    io.poll_error = EIO;
    assert(readline_getc(&v.vtbl) == EOF && errno == EIO);
    assert_gate_free();
    enqueue('b'); io.read_error = EIO;
    assert(readline_getc(&v.vtbl) == EOF && errno == EIO);
    assert_gate_free();
    io.length = 0; io.eof = true;
    assert(readline_getc(&v.vtbl) == EOF);
    assert_gate_free();
    /* A stale/read-gate holder cannot turn claim into an infinite wait. */
    reset_io(); io.fake_time = true;
    assert(zhaoxi_console_readline_begin(CONSOLE) == 1);
    assert(zhaoxi_console_claim() == -ETIMEDOUT && io.now == 250);
    assert(zhaoxi_console_release() == -EPERM);
    zhaoxi_console_readline_end();
    assert_gate_free();
    io.clock_error = EIO;
    assert(zhaoxi_console_claim() == -EIO);
    io.clock_error = 0;
    assert_gate_free();
    assert(zhaoxi_console_readline_begin(CONSOLE) == 1);
    io.sleep_error = EIO;
    assert(zhaoxi_console_claim() == -EIO);
    zhaoxi_console_readline_end();
    assert_gate_free();
    assert(zhaoxi_console_readline_begin(CONSOLE) == 1);
    io.sleep_error = EINTR;
    assert(zhaoxi_console_claim() == -ETIMEDOUT);
    zhaoxi_console_readline_end();
    assert_gate_free();
    /* Unrelated tty keeps the original blocking path even during PPP. */
    reset_io();
    assert(zhaoxi_console_claim() == 0);
    enqueue('u'); v = fd_vtbl(OTHER_TTY);
    assert(readline_getc(&v.vtbl) == 'u' && io.polls == 0);
#ifdef CONFIG_READLINE_ECHO
    readline_putc(&v.vtbl, 'u');
    readline_write(&v.vtbl, "ok", 2);
    assert(io.writes == 3);
#endif
    assert(zhaoxi_console_release() == 0);
}
static void *one_getc(void *result)
{
    struct readline_s v = fd_vtbl(CONSOLE);
    *(int *)result = readline_getc(&v.vtbl);
    return NULL;
}
static void *pending_claim(void *unused)
{
    (void)unused;
    assert(zhaoxi_console_claim() == 0);
    assert(zhaoxi_console_release() == 0);
    return NULL;
}
static void poll_read_race(void)
{
    reset_io(); enqueue('r'); io.freeze_read = true;
    pthread_t reader, claimant;
    int value = 0;
    assert(pthread_create(&reader, NULL, one_getc, &value) == 0);
    wait_counter(&io.read_enters, 1);
    assert(pthread_create(&claimant, NULL, pending_claim, NULL) == 0);
    /* Wait until the claimant published its intent, then verify it cannot
     * acquire the gate while the poll-confirmed read is in flight. */
    struct timespec limit;
    assert(clock_gettime(CLOCK_MONOTONIC, &limit) == 0);
    while (!atomic_load(&pending)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec - limit.tv_sec < 2);
        sched_yield();
    }
    assert(!atomic_load(&owned));
    assert(zhaoxi_console_readline_begin(CONSOLE) == -EAGAIN);
    pthread_mutex_lock(&io_lock);
    io.freeze_read = false;
    pthread_cond_broadcast(&io_event);
    pthread_mutex_unlock(&io_lock);
    assert(pthread_join(reader, NULL) == 0 && value == 'r');
    assert(pthread_join(claimant, NULL) == 0);
    assert_gate_free();
}
#endif

int main(void)
{
    (void)fake_write;
#if !defined(NO_PRODUCT_HOOK) && !defined(PARTIAL_PRODUCT_HOOK)
    idle_handoff();
    identity_and_errors();
    poll_read_race();
#else
    /* Weak hook absence and a partial pair must never enable poll/gating. */
    reset_io();
    assert(zhaoxi_console_claim() == 0);
    enqueue('n');
    struct readline_s v = fd_vtbl(CONSOLE);
    assert(readline_getc(&v.vtbl) == 'n' && io.polls == 0);
    readline_putc(&v.vtbl, 'n');
    readline_write(&v.vtbl, "old", 3);
    assert(io.writes == 4 && io.polls == 0);
    io.eof = true;
    assert(readline_getc(&v.vtbl) == EOF);
    assert(zhaoxi_console_release() == 0);
    /* Keep synchronization helpers referenced in these configurations. */
    (void)wait_counter;
#endif
    puts("console lease: ownership, isolation, restoration and cleanup passed");
    return 0;
}

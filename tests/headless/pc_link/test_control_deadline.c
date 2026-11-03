/* SPDX-License-Identifier: Apache-2.0 */
/* Complete production control implementation + real cJSON. All socket/time
 * operations are doubles: this test opens no socket and uses no real network. */
#include "cJSON.h"
#include "morning.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LISTENER 41
#define CLIENT 42
static struct {
    int64_t now, next_rx, next_tx, rx_period, tx_period, stop_at, down_at;
    const char *input;
    size_t used, rx_fragment, tx_fragment, sent;
    char output[4096];
    int flags, fcntl_failure, poll_error, recv_error, send_error;
    unsigned polls, receives, sends, client_closed, listener_closed, drains;
    int64_t closed_at, drained_at;
    bool accepted, finish_on_close, hup, clock_failure;
} io;

static void reset(const char *input)
{
    memset(&io, 0, sizeof(io));
    io.input = input; io.rx_fragment = io.tx_fragment = 4096;
    io.stop_at = io.down_at = INT64_MAX;
    io.flags = O_APPEND;
    io.finish_on_close = true;
}
static bool zhaoxi_pc_link_stopping(void)
{ return io.now >= io.stop_at || (io.finish_on_close && io.client_closed); }
static bool zhaoxi_pc_link_alive(void) { return io.now < io.down_at; }
static int fake_clock_gettime(clockid_t id, struct timespec *out)
{
    assert(id == CLOCK_MONOTONIC);
    if (io.clock_failure) { errno = EIO; return -1; }
    out->tv_sec = io.now / 1000; out->tv_nsec = io.now % 1000 * 1000000;
    return 0;
}
static int fake_socket(int domain, int type, int protocol)
{ assert(domain == AF_INET && type == SOCK_STREAM && !protocol); return LISTENER; }
static int fake_setsockopt(int fd, int level, int name, const void *value, socklen_t size)
{
    assert(fd == LISTENER && level == SOL_SOCKET && name == SO_REUSEADDR);
    assert(size == sizeof(int) && *(const int *)value == 1); return 0;
}
static int fake_bind(int fd, const struct sockaddr *addr, socklen_t size)
{
    const struct sockaddr_in *local = (const struct sockaddr_in *)addr;
    assert(fd == LISTENER && size == sizeof(*local) && ntohs(local->sin_port) == 8765);
    assert(local->sin_addr.s_addr == inet_addr("10.254.42.2")); return 0;
}
static int fake_listen(int fd, int backlog) { assert(fd == LISTENER && backlog == 2); return 0; }
static int fake_accept(int fd, struct sockaddr *addr, socklen_t *size)
{
    assert(fd == LISTENER && !io.accepted && *size == sizeof(struct sockaddr_in));
    struct sockaddr_in *peer = (struct sockaddr_in *)addr;
    memset(peer, 0, sizeof(*peer)); peer->sin_addr.s_addr = inet_addr("10.254.42.1");
    io.accepted = true; return CLIENT;
}
static int fake_fcntl(int fd, int cmd, ...)
{
    assert(fd == CLIENT);
    if (io.fcntl_failure == cmd) { errno = EIO; return -1; }
    if (cmd == F_GETFL) return io.flags;
    assert(cmd == F_SETFL);
    va_list args; va_start(args, cmd); io.flags = va_arg(args, int); va_end(args);
    assert((io.flags & (O_APPEND | O_NONBLOCK)) == (O_APPEND | O_NONBLOCK)); return 0;
}
static int fake_close(int fd)
{
    if (fd == CLIENT) { assert(!io.client_closed); io.client_closed++; io.closed_at = io.now; }
    else { assert(fd == LISTENER && !io.listener_closed); io.listener_closed++; }
    return 0;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && timeout > 0 && timeout <= 100); io.polls++;
    if (io.poll_error) { errno = io.poll_error; io.poll_error = 0; io.now++; return -1; }
    if (fds->fd == LISTENER) { fds->revents = POLLIN; return 1; }
    assert(fds->fd == CLIENT && (io.flags & O_NONBLOCK));
    assert(fds->events == POLLIN || fds->events == POLLOUT);
    if (io.hup) { fds->revents = POLLHUP; return 1; }
    int64_t next = fds->events == POLLIN ? io.next_rx : io.next_tx;
    if (fds->events == POLLIN && !io.input[io.used]) next = INT64_MAX;
    int64_t until = io.now + timeout;
    if (next < until) until = next;
    if (until < io.now) until = io.now;
    if (io.stop_at > io.now && io.stop_at < until) until = io.stop_at;
    if (io.down_at > io.now && io.down_at < until) until = io.down_at;
    io.now = until;
    fds->revents = io.now >= next ? fds->events : 0;
    return fds->revents ? 1 : 0;
}
static ssize_t fake_recv(int fd, void *out, size_t capacity, int flags)
{
    assert(fd == CLIENT && !flags && (io.flags & O_NONBLOCK)); io.receives++;
    if (io.recv_error) { errno = io.recv_error; io.recv_error = 0; io.next_rx = io.now + 1; return -1; }
    size_t count = strlen(io.input + io.used);
    if (count > capacity) count = capacity;
    if (count > io.rx_fragment) count = io.rx_fragment;
    memcpy(out, io.input + io.used, count); io.used += count;
    io.next_rx = io.now + io.rx_period; return (ssize_t)count;
}
static ssize_t fake_send(int fd, const void *bytes, size_t length, int flags)
{
    assert(fd == CLIENT && !flags && (io.flags & O_NONBLOCK)); io.sends++;
    if (io.send_error) { errno = io.send_error; io.send_error = 0; io.next_tx = io.now + 1; return -1; }
    if (length > io.tx_fragment) length = io.tx_fragment;
    assert(io.sent + length < sizeof(io.output));
    memcpy(io.output + io.sent, bytes, length); io.sent += length; io.output[io.sent] = 0;
    io.next_tx = io.now + io.tx_period; return (ssize_t)length;
}
static int fake_usleep(useconds_t us)
{
    assert(us == 250000 && io.client_closed && io.sent && io.output[io.sent - 1] == '\n');
    io.drains++; io.drained_at = io.now; io.now += us / 1000; return 0;
}
static int netlib_getifstatus(const char *name, uint8_t *flags)
{ assert(!strcmp(name, "ppp0")); *flags = IFF_UP | IFF_RUNNING; return 0; }
static void voice_channel_auto_stop(void) {}
static int voice_channel_auto_status(char *out, size_t size) { (void)out; (void)size; return -ENOSYS; }
static int zhaoxi_note_path(time_t now, char *out, size_t size) { (void)now; (void)out; (void)size; return -ENOSYS; }
static int tool_registry_execute(const char *tool, const char *input, char *out, size_t size)
{ (void)tool; (void)input; (void)out; (void)size; return -ENOSYS; }
int zhaoxi_main(int argc, char **argv) { (void)argc; (void)argv; return -ENOSYS; }
int zhaoxi_morning_sync_time(int64_t epoch, const char *source) { (void)epoch; (void)source; return -ENOSYS; }
int zhaoxi_morning_set_profile(const char *text) { (void)text; return -ENOSYS; }
int morning_profile_get(char *out, size_t cap) { (void)out; (void)cap; return -ENOSYS; }
int zhaoxi_morning_status(char *out, size_t cap) { (void)out; (void)cap; return -ENOSYS; }
int morning_day(int64_t epoch, char date[11], unsigned *minute) { (void)epoch; (void)date; (void)minute; return -ENOSYS; }
int morning_batch_load(const char *day, struct morning_batch *batch) { (void)day; (void)batch; return -ENOSYS; }
int morning_batch_render(const struct morning_batch *batch, char *out, size_t cap)
{ (void)batch; (void)out; (void)cap; return -ENOSYS; }

#define clock_gettime fake_clock_gettime
#define socket fake_socket
#define setsockopt fake_setsockopt
#define bind fake_bind
#define listen fake_listen
#define accept fake_accept
#define fcntl fake_fcntl
#define close fake_close
#define poll fake_poll
#define recv fake_recv
#define send fake_send
#define usleep fake_usleep
/* ACTUAL_PC_CONTROL */

static void nonblocking(void) { io.flags |= O_NONBLOCK; io.finish_on_close = false; }
static void assert_closed(int64_t time)
{ assert(io.client_closed == 1 && io.listener_closed == 1 && io.closed_at == time); }
static void assert_reply(bool ending)
{
    assert(io.sent && io.output[io.sent - 1] == '\n');
    cJSON *reply = cJSON_Parse(io.output); assert(reply);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(reply, "rc")) == 0);
    if (ending) assert(cJSON_IsTrue(cJSON_GetObjectItem(reply, "ending")));
    else assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "transport")),
                        "native BES PPP via computer"));
    cJSON_Delete(reply);
}
int main(void)
{
    /* One byte every second never hits the old per-recv five-second timeout. */
    reset("{\"action\":\"ping\"}\n"); io.rx_fragment = 1; io.rx_period = 1000;
    assert(zhaoxi_pc_control() == -ECANCELED); assert_closed(5000);
    assert(io.used == 5 && !io.sent && !io.drains);
    reset("{\"action\":\"ping\"}\n"); io.tx_fragment = 1; io.tx_period = 1000;
    assert(zhaoxi_pc_control() == -ECANCELED); assert_closed(5000);
    assert(io.sent == 5 && !io.drains);
    puts("PASS: receive/send byte trickles close accepted fd at absolute 5000 ms");

    /* Idle I/O and slow-progress I/O both observe stop/link loss in one slice. */
    for (int sending = 0; sending < 2; sending++) {
        for (int down = 0; down < 2; down++) {
            reset(sending ? "{\"action\":\"ping\"}\n" : "");
            if (sending) io.next_tx = 10000;
            if (down) io.down_at = 251; else io.stop_at = 251;
            assert(zhaoxi_pc_control() == (down ? -ENETDOWN : -ECANCELED));
            assert(io.closed_at >= 251 && io.closed_at <= 351);
            assert_closed(io.closed_at); assert(!io.sent && !io.drains);
        }
    }
    reset("{\"action\":\"quit\"}\n"); io.next_tx = 10000; io.stop_at = 251;
    assert(zhaoxi_pc_control() == -ECANCELED); assert_closed(251); assert(!io.drains);
    puts("PASS: receive/send stop and link loss interrupt within 100 ms; quit abort skips drain");

    reset("{\"action\":\"ping\"}\n"); io.rx_fragment = io.tx_fragment = 1;
    assert(zhaoxi_pc_control() == -ECANCELED); assert_closed(0); assert_reply(false); assert(!io.drains);
    reset("{\"action\":\"quit\"}\n"); io.rx_fragment = io.tx_fragment = 1; io.tx_period = 2;
    assert(zhaoxi_pc_boot_control() == 0); assert_reply(true);
    assert(io.drains == 1 && io.drained_at == io.closed_at && io.now == io.closed_at + 250);
    reset("{\"action\":\"quit\"}\n"); io.tx_fragment = 1; io.tx_period = 1000;
    assert(zhaoxi_pc_control() == 0); assert_closed(5000); assert(io.sent == 5 && !io.drains);
    puts("PASS: actual ping/quit dispatch, fragmented full reply + newline, 250 ms drain only after success");

    char input[2048];
    for (size_t i = 0; i < 3; i++) {
        const int transient[] = {EINTR, EAGAIN, EWOULDBLOCK};
        reset("{\"action\":\"ping\"}\n"); nonblocking(); io.recv_error = transient[i];
        assert(pc_control_receive(CLIENT, input, sizeof(input)) == 0);
        reset(""); nonblocking(); io.send_error = transient[i];
        assert(pc_control_send(CLIENT, "ok") == 0 && !strcmp(io.output, "ok\n"));
    }
    reset("{\"action\":\"ping\"}\n"); nonblocking(); io.poll_error = EINTR;
    assert(pc_control_receive(CLIENT, input, sizeof(input)) == 0);
    reset(""); nonblocking(); io.tx_fragment = 1; io.tx_period = 1000;
    assert(pc_control_send(CLIENT, "abcde") == -ETIMEDOUT);
    assert(io.now == 5000 && !strcmp(io.output, "abcde")); /* newline shares deadline */
    reset(""); nonblocking(); io.clock_failure = true;
    assert(pc_control_receive(CLIENT, input, sizeof(input)) == -EIO);
    reset(""); nonblocking(); io.hup = true;
    assert(pc_control_receive(CLIENT, input, sizeof(input)) == -ECONNRESET);
    reset("bad\n"); io.fcntl_failure = F_GETFL;
    assert(zhaoxi_pc_control() == -ECANCELED); assert_closed(0); assert(!io.receives);
    reset("bad\n"); io.fcntl_failure = F_SETFL;
    assert(zhaoxi_pc_control() == -ECANCELED); assert_closed(0); assert(!io.receives);
    reset("bad\n"); nonblocking(); io.recv_error = ECONNRESET;
    assert(pc_control_receive(CLIENT, input, sizeof(input)) == -ECONNRESET);
    reset(""); nonblocking(); io.send_error = EPIPE;
    assert(pc_control_send(CLIENT, "ok") == -EPIPE);
    puts("PASS: transient readiness races, EINTR, framing deadline, clock/socket/fcntl failure paths");
    return 0;
}

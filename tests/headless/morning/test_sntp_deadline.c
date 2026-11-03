/* SPDX-License-Identifier: Apache-2.0 */
/* Production functions are inserted verbatim by check_sntp_deadline.py.
 * Every external I/O/time dependency is a double; no real sockets or DNS. */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FD 42
#define EXPECTED_EPOCH INT64_C(1793692800)
#define UNCHANGED_EPOCH INT64_C(-123456789)
enum reply_kind { VALID, FORGED_ORIGINATE, SHORT_PACKET, EMPTY_PACKET,
                  BAD_MODE, UNSYNCHRONIZED, ZERO_STRATUM, LARGE_STRATUM };
static struct {
    int64_t now, stop_at, send_ready_at;
    int flags, fcntl_failure, socket_error, connect_error, random_error;
    int send_error, recv_error, poll_error, day_error;
    unsigned polls, sends, receives, closes, resolves, frees, sockets;
    unsigned poll_eintr, send_eintr, recv_eintr, send_eagain, recv_eagain;
    unsigned getflags, setflags, days;
    int step_ms, short_send;
    bool send_forever, recv_forever, poll_forever_eintr, block_send, block_recv;
    bool resolve_error, sent;
    enum reply_kind reply_kind;
    unsigned char request[48];
} io;
static struct sockaddr_in peer;
static struct addrinfo address;

static void reset(void)
{
    memset(&io, 0, sizeof(io));
    io.flags = O_APPEND; /* Existing status flags must survive F_SETFL. */
    io.stop_at = INT64_MAX;
    io.step_ms = 10;
    io.short_send = -1;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(123);
    peer.sin_addr.s_addr = htonl(UINT32_C(0xc0000201));
    memset(&address, 0, sizeof(address));
    address.ai_addr = (struct sockaddr *)&peer;
    address.ai_addrlen = sizeof(peer);
}
static int agent_shutdown_requested(void) { return io.now >= io.stop_at; }
static int fake_clock_gettime(clockid_t clock, struct timespec *time)
{
    assert(clock == CLOCK_MONOTONIC);
    time->tv_sec = io.now / 1000;
    time->tv_nsec = (io.now % 1000) * 1000000;
    return 0;
}
static int fake_getaddrinfo(const char *host, const char *service,
                            const struct addrinfo *hints, struct addrinfo **result)
{
    assert(!strcmp(host, "ntp.aliyun.com") && !strcmp(service, "123"));
    assert(hints->ai_family == AF_INET && hints->ai_socktype == SOCK_DGRAM);
    io.resolves++;
    if (io.resolve_error) return EAI_FAIL;
    *result = &address;
    return 0;
}
static void fake_freeaddrinfo(struct addrinfo *value)
{ assert(value == &address && !io.frees); io.frees++; }
static int fake_socket(int family, int type, int protocol)
{
    assert(family == AF_INET && type == SOCK_DGRAM && !protocol);
    io.sockets++;
    if (io.socket_error) { errno = io.socket_error; return -1; }
    return FD;
}
static int fake_connect(int fd, const struct sockaddr *value, socklen_t size)
{
    assert(fd == FD && value == address.ai_addr && size == sizeof(peer));
    if (io.connect_error) { errno = io.connect_error; return -1; }
    return 0;
}
static int agent_secure_random(void *out, size_t size)
{
    assert(size == 8);
    if (io.random_error) return -1;
    for (size_t i = 0; i < size; i++) ((unsigned char *)out)[i] = (unsigned char)(0xa1 + i);
    return 0;
}
static int fake_fcntl(int fd, int command, ...)
{
    assert(fd == FD && !io.closes);
    if (command == F_GETFL) io.getflags++;
    else { assert(command == F_SETFL); io.setflags++; }
    if (io.fcntl_failure == command) { errno = EACCES; return -1; }
    if (command == F_GETFL) return io.flags;
    va_list args;
    va_start(args, command);
    io.flags = va_arg(args, int);
    va_end(args);
    assert((io.flags & (O_APPEND | O_NONBLOCK)) == (O_APPEND | O_NONBLOCK));
    return 0;
}
static int fake_close(int fd)
{ assert(fd == FD && !io.closes); io.closes++; return 0; }
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds->fd == FD && !io.closes);
    assert(fds->events == POLLIN || fds->events == POLLOUT);
    assert(timeout > 0 && timeout <= 100 && (io.flags & O_NONBLOCK));
    assert(++io.polls < 10000); /* A deadline regression cannot hang this test. */
    bool sending = fds->events == POLLOUT;
    bool blocked = sending ? io.block_send : io.block_recv;
    int advance = blocked ? timeout : io.step_ms;
    if (advance > timeout) advance = timeout;
    io.now += advance; /* Stop does not wake poll: exercise the full slice bound. */
    if (io.poll_forever_eintr || io.poll_eintr) {
        if (io.poll_eintr) io.poll_eintr--;
        errno = EINTR; return -1;
    }
    if (io.poll_error) { errno = io.poll_error; io.poll_error = 0; return -1; }
    fds->revents = !blocked && (!sending || io.now >= io.send_ready_at) ? fds->events : 0;
    return fds->revents ? 1 : 0;
}
static ssize_t fake_send(int fd, const void *data, size_t size, int flags)
{
    assert(fd == FD && !flags && size == sizeof(io.request) && !io.closes);
    assert(io.flags & O_NONBLOCK); /* Never permit a blocking send after poll. */
    io.sends++;
    assert(((const unsigned char *)data)[0] == 0x23);
    if (io.send_eintr) { io.send_eintr--; errno = EINTR; return -1; }
    if (io.send_forever || io.send_eagain) {
        if (io.send_eagain) io.send_eagain--;
        errno = EAGAIN; return -1;
    }
    if (io.send_error) { errno = io.send_error; return -1; }
    if (io.short_send >= 0) return io.short_send;
    assert(!io.sent);
    memcpy(io.request, data, size);
    for (int i = 0; i < 8; i++) assert(io.request[40 + i] == (unsigned char)(0xa1 + i));
    io.sent = true;
    return (ssize_t)size;
}
static ssize_t fake_recv(int fd, void *out, size_t capacity, int flags)
{
    assert(fd == FD && !flags && capacity == 48 && io.sent && !io.closes);
    assert(io.flags & O_NONBLOCK); /* Core regression guard: recv must not block. */
    io.receives++;
    if (io.recv_eintr) { io.recv_eintr--; errno = EINTR; return -1; }
    if (io.recv_forever || io.recv_eagain) {
        if (io.recv_eagain) io.recv_eagain--;
        errno = EAGAIN; return -1;
    }
    if (io.recv_error) { errno = io.recv_error; return -1; }
    unsigned char reply[48] = {0};
    reply[0] = 0x24; reply[1] = 2; /* NTP v4 server, synchronized stratum 2. */
    memcpy(reply + 24, io.request + 40, 8);
    uint32_t seconds = htonl((uint32_t)(EXPECTED_EPOCH + INT64_C(2208988800)));
    memcpy(reply + 40, &seconds, sizeof(seconds));
    switch (io.reply_kind) {
    case FORGED_ORIGINATE: reply[24] ^= 1; break;
    case BAD_MODE: reply[0] = 0x23; break;
    case UNSYNCHRONIZED: reply[0] |= 0xc0; break;
    case ZERO_STRATUM: reply[1] = 0; break;
    case LARGE_STRATUM: reply[1] = 16; break;
    default: break;
    }
    size_t length = io.reply_kind == SHORT_PACKET ? 47 :
                    io.reply_kind == EMPTY_PACKET ? 0 : sizeof(reply);
    memcpy(out, reply, length);
    return (ssize_t)length;
}
static int morning_day(int64_t epoch, char day[11], unsigned *minute)
{
    assert(epoch == EXPECTED_EPOCH && !minute);
    io.days++;
    if (io.day_error) return io.day_error;
    memcpy(day, "2026-11-03", 11);
    return 0;
}

#define clock_gettime fake_clock_gettime
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define socket fake_socket
#define connect fake_connect
#define fcntl fake_fcntl
#define close fake_close
#define poll fake_poll
#define send fake_send
#define recv fake_recv
/* ACTUAL_MORNING_SNTP */

static void expect_result(int result)
{
    int64_t epoch = UNCHANGED_EPOCH;
    assert(morning_sntp(&epoch) == result);
    assert(epoch == (result ? UNCHANGED_EPOCH : EXPECTED_EPOCH));
    assert(io.resolves == 1);
    assert(io.frees == (io.resolve_error ? 0U : 1U));
    assert(io.closes == (io.resolve_error || io.socket_error ? 0U : 1U));
}
int main(void)
{
    reset();
    assert(morning_sntp(NULL) == -EINVAL && !io.resolves && !io.sockets);
    expect_result(0);
    assert(io.getflags == 1 && io.setflags == 1 && io.days == 1);
    assert(io.sends == 1 && io.receives == 1);
    puts("PASS: actual SNTP accepts a matching NTP response and converts its epoch");

    reset(); io.recv_forever = true;
    expect_result(-ETIMEDOUT);
    assert(io.now == 3000 && io.receives > 100 && io.sends == 1);
    puts("PASS: spurious readable + recv EAGAIN exits at the shared 3000 ms deadline");

    reset(); io.send_forever = true;
    expect_result(-ETIMEDOUT);
    assert(io.now == 3000 && io.sends > 100 && !io.receives);
    reset(); io.send_eagain = 249; io.recv_forever = true;
    expect_result(-ETIMEDOUT);
    assert(io.now == 3000 && io.sends == 250 && io.receives == 50);
    reset(); io.send_eagain = 249;
    expect_result(0);
    assert(io.now == 2510 && io.sends == 250 && io.receives == 1);
    puts("PASS: send EAGAIN retries; 2500 ms send + receive share one 3000 ms budget");

    reset(); io.poll_eintr = 3; io.send_eintr = 2; io.recv_eintr = 4;
    io.send_eagain = 2; io.recv_eagain = 3;
    expect_result(0);
    assert(io.sends == 5 && io.receives == 8 && io.now == 160);
    reset(); io.poll_forever_eintr = true;
    expect_result(-ETIMEDOUT);
    assert(io.now == 3000 && !io.sends && !io.receives);
    puts("PASS: poll/send/recv EINTR retries and repeated EINTR retain the deadline");

    for (int sending = 0; sending < 2; sending++) {
        for (int spurious = 0; spurious < 2; spurious++) {
            reset(); io.stop_at = 251;
            if (sending) { io.block_send = !spurious; io.send_forever = spurious; }
            else { io.block_recv = !spurious; io.recv_forever = spurious; }
            expect_result(-ECANCELED);
            assert(io.now >= io.stop_at && io.now - io.stop_at <= 100);
        }
    }
    reset(); io.stop_at = 0;
    expect_result(-ECANCELED);
    assert(!io.polls && !io.sends && !io.receives && io.now == 0);
    puts("PASS: stop observed within 100 ms during send/receive idle or EAGAIN loops");

    const enum reply_kind invalid[] = { FORGED_ORIGINATE, SHORT_PACKET, EMPTY_PACKET,
        BAD_MODE, UNSYNCHRONIZED, ZERO_STRATUM, LARGE_STRATUM };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); io.reply_kind = invalid[i];
        expect_result(-EPROTO);
        assert(!io.days && io.receives == 1);
    }
    reset(); io.day_error = -ERANGE; expect_result(-ERANGE);
    puts("PASS: forged originate, short/empty packets, invalid NTP metadata rejected");

    const int commands[] = { F_GETFL, F_SETFL };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        reset(); io.fcntl_failure = commands[i];
        expect_result(-EACCES);
        assert(!io.polls && !io.sends && !io.receives && io.now == 0);
        assert(io.getflags == 1 && io.setflags == (commands[i] == F_SETFL ? 1U : 0U));
    }
    reset(); io.resolve_error = true; expect_result(-EHOSTUNREACH);
    reset(); io.socket_error = EMFILE; expect_result(-EMFILE);
    reset(); io.connect_error = ENETUNREACH; expect_result(-ENETUNREACH);
    reset(); io.random_error = EIO; expect_result(-EIO);
    reset(); io.poll_error = EBADF; expect_result(-EBADF);
    reset(); io.send_error = EPIPE; expect_result(-EPIPE);
    reset(); io.recv_error = ECONNREFUSED; expect_result(-ECONNREFUSED);
    reset(); io.short_send = 47; expect_result(-EIO); assert(!io.receives);
    puts("PASS: fcntl failures close fd without I/O; setup and I/O errors preserve epoch");
    puts("PASS: all send/recv calls asserted O_NONBLOCK; all descriptors closed exactly once");
    return 0;
}

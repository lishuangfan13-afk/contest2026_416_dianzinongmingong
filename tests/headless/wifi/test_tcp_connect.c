#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct { int fd; } mbedtls_net_context;
#define MBEDTLS_ERR_NET_UNKNOWN_HOST -0x52
#define MBEDTLS_ERR_NET_CONNECT_FAILED -0x44
static int scenario, closes, selects, restored, fd_next;
static int64_t elapsed_ms;
static struct sockaddr_in peer;
static struct addrinfo address;

static int fake_getaddrinfo(const char *host, const char *port,
                            const struct addrinfo *hints, struct addrinfo **result)
{
    assert(!strcmp(host, "test.invalid") && !strcmp(port, "443"));
    assert(hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM);
    if (scenario == 4) return EAI_FAIL;
    address = (struct addrinfo){ .ai_family=AF_INET, .ai_socktype=SOCK_STREAM,
        .ai_protocol=IPPROTO_TCP, .ai_addr=(struct sockaddr *)&peer,
        .ai_addrlen=sizeof(peer) };
    *result = &address;
    return 0;
}
static void fake_freeaddrinfo(struct addrinfo *p) { assert(p == &address); }
static int fake_socket(int family, int type, int proto)
{
    assert(family == AF_INET && type == SOCK_STREAM && proto == IPPROTO_TCP);
    return fd_next++;
}
static int fake_fcntl(int fd, int operation, int flags)
{
    assert(fd == 5);
    if (operation == F_GETFL) return O_RDWR;
    assert(operation == F_SETFL);
    if (!(flags & O_NONBLOCK)) restored++;
    return 0;
}
static int fake_connect(int fd, const struct sockaddr *a, socklen_t size)
{
    assert(fd == 5 && a == (struct sockaddr *)&peer && size == sizeof(peer));
    if (scenario == 0) return 0;
    errno = EINPROGRESS;
    return -1;
}
static int fake_close(int fd) { assert(fd == 5); closes++; return 0; }
static int fake_clock_gettime(clockid_t clock, struct timespec *at)
{
    assert(clock == CLOCK_MONOTONIC);
    at->tv_sec = 100 + elapsed_ms / 1000;
    at->tv_nsec = (elapsed_ms % 1000) * 1000000;
    return 0;
}
static int fake_select(int nfds, fd_set *readfds, fd_set *writefds,
                        fd_set *exceptfds, struct timeval *timeout)
{
    assert(nfds == 6 && !readfds && !exceptfds && FD_ISSET(5, writefds));
    selects++;
    if (scenario == 2) { assert(timeout->tv_sec == 15); return 0; }
    if (scenario == 3 && selects == 1) {
        elapsed_ms += 7000; errno=EINTR; return -1;
    }
    if (scenario == 3) assert(timeout->tv_sec == 8);
    return 1;
}
static int fake_getsockopt(int fd, int level, int name, void *value, socklen_t *size)
{
    assert(fd == 5 && level == SOL_SOCKET && name == SO_ERROR && *size == sizeof(int));
    *(int *)value = scenario == 1 ? ECONNREFUSED : 0;
    return 0;
}

#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define socket fake_socket
#define fcntl fake_fcntl
#define connect fake_connect
#define close fake_close
#define clock_gettime fake_clock_gettime
#define select fake_select
#define getsockopt fake_getsockopt
#include "tcp_connect_helper.c"

int main(void)
{
    for (scenario = 0; scenario < 5; scenario++) {
        closes=selects=restored=0; fd_next=5; elapsed_ms=0;
        mbedtls_net_context ctx = { .fd=-1 };
        int result = zhaoxi_tcp_connect(&ctx, "test.invalid", "443", 15000);
        if (scenario == 0 || scenario == 3) {
            assert(result == 0 && ctx.fd == 5 && closes == 0 && restored == 1);
        } else {
            assert(result < 0 && ctx.fd == -1 && restored == 0);
            assert(closes == (scenario == 4 ? 0 : 1));
        }
    }
    puts("PASS: TCP immediate success, SO_ERROR refusal, timeout, EINTR remaining budget, DNS failure and descriptor cleanup. POSIX boundary fixture only.");
}

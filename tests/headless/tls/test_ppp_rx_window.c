/* Real helper code is inserted below; all operating-system calls are fakes. */
#include <assert.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>

static const char *TAG = "offline-fixture";
static unsigned checks;
static struct {
    const char *value;
    int config_rc;
    unsigned config_calls, if_calls, free_calls, set_calls, get_calls;
    struct ifaddrs interfaces[3];
    struct sockaddr_in addresses[3];
    struct ifaddrs *head;
    int if_rc, set_rc, get_rc, actual;
    socklen_t actual_size;
    int set_errno, get_errno, requested;
    unsigned logs, warnings, infos;
    char log[4096];
} f;

static int fixture_config_get(const char *key, char *buffer, size_t size)
{
    assert(!strcmp(key, "ppp_tls_rx_window"));
    f.config_calls++;
    errno = EACCES;
    if (f.config_rc) return f.config_rc;
    assert(f.value != NULL && size > 0);
    (void)snprintf(buffer, size, "%s", f.value);
    return 0;
}

static int fixture_getifaddrs(struct ifaddrs **out)
{
    f.if_calls++;
    errno = ENODEV;
    if (f.if_rc) return f.if_rc;
    *out = f.head;
    return 0;
}

static void fixture_freeifaddrs(struct ifaddrs *interfaces)
{
    assert(interfaces == f.head);
    f.free_calls++;
    errno = EINVAL;
}

static int fixture_setsockopt(int fd, int level, int option,
                             const void *value, socklen_t size)
{
    assert(fd == 42 && level == SOL_SOCKET && option == SO_RCVBUF);
    assert(size == sizeof(int));
    f.requested = *(const int *)value;
    f.set_calls++;
    errno = f.set_errno;
    return f.set_rc;
}

static int fixture_getsockopt(int fd, int level, int option,
                             void *value, socklen_t *size)
{
    assert(fd == 42 && level == SOL_SOCKET && option == SO_RCVBUF);
    assert(*size == sizeof(int));
    f.get_calls++;
    errno = f.get_errno;
    if (f.get_rc) return f.get_rc;
    *(int *)value = f.actual < 0 ? f.requested : f.actual;
    *size = f.actual_size;
    return 0;
}

static void fixture_syslog(int priority, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(f.log, sizeof(f.log), format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(f.log));
    f.logs++;
    f.warnings += priority == LOG_WARNING;
    f.infos += priority == LOG_INFO;
    errno = E2BIG; /* Logging must not replace the caller's socket errno. */
}

#define claw_config_get fixture_config_get
#define getifaddrs fixture_getifaddrs
#define freeifaddrs fixture_freeifaddrs
#define setsockopt fixture_setsockopt
#define getsockopt fixture_getsockopt
#define syslog fixture_syslog

/* ACTUAL_PPP_RX_WINDOW_HELPERS */

static void reset(void)
{
    memset(&f, 0, sizeof(f));
    f.value = "0";
    f.actual = -1;
    f.actual_size = sizeof(int);
    f.set_errno = ENOBUFS;
    f.get_errno = ENOTSOCK;
    for (unsigned i = 0; i < 3; i++) {
        f.interfaces[i].ifa_name = (char *)(i == 0 ? "ppp0" : "eth0");
        f.interfaces[i].ifa_flags = IFF_UP | IFF_RUNNING;
        f.interfaces[i].ifa_addr = (struct sockaddr *)&f.addresses[i];
        f.addresses[i].sin_family = AF_INET;
        f.addresses[i].sin_addr.s_addr = htonl(0xc000022a);
    }
    f.head = &f.interfaces[0];
    errno = EBUSY;
}

static void apply(void)
{
    errno = EBUSY;
    zhaoxi_apply_ppp_rx_window(42);
    assert(errno == EBUSY);
    assert(f.config_calls == 1);
    assert(f.set_calls <= 1 && f.get_calls <= 1);
    assert(!strstr(f.log, "PRIVATE_CONFIG_VALUE"));
    assert(!strstr(f.log, "192.0.2.42"));
    checks++;
}

static void assert_disabled(void)
{
    apply();
    assert(!f.set_calls && !f.get_calls && !f.infos);
}

int main(void)
{
    reset(); f.config_rc = -1; f.value = NULL;
    assert_disabled(); assert(!f.if_calls && !f.logs);
    reset(); assert_disabled(); assert(!f.if_calls && !f.logs);
    /* config_store rejects numeric JSON values through this same rc path. */
    reset(); f.config_rc = -1; f.value = "2048";
    assert_disabled(); assert(!f.if_calls && !f.logs);

    const char *invalid[] = {"", " 2048", "2048 ", "\t2048", "2048\n",
        "+2048", "-2048", "2048.0", "2e3", "02048", "2049", "1", "1024",
        "8193", "16384", "-1", "4294967296", "0x800", "00", "PRIVATE_CONFIG_VALUE"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset(); f.value = invalid[i]; assert_disabled();
        assert(!f.if_calls && f.warnings == 1);
        assert(strstr(f.log, "config=invalid; default kept"));
    }

    const char *valid[] = {"2048", "4096", "8192"};
    const int expected[] = {2048, 4096, 8192};
    for (unsigned i = 0; i < 3; i++) {
        reset(); f.value = valid[i]; apply();
        assert(f.if_calls == 1 && f.free_calls == 1);
        assert(f.set_calls == 1 && f.get_calls == 1);
        assert(f.requested == expected[i] && f.infos == 1 && !f.warnings);
        char wanted[80];
        (void)snprintf(wanted, sizeof(wanted), "requested=%d actual=%d set_rc=0 set_errno=0",
                       expected[i], expected[i]);
        assert(strstr(f.log, wanted));
        assert(strstr(f.log, "get_rc=0 get_errno=0"));
    }

    /* The helper must scan past unrelated or invalid interfaces. */
    reset(); f.value = "2048";
    f.interfaces[0].ifa_name = (char *)"eth0";
    f.interfaces[0].ifa_next = &f.interfaces[1];
    f.interfaces[1].ifa_name = (char *)"ppp0";
    apply(); assert(f.set_calls == 1 && f.free_calls == 1);

    for (unsigned mode = 0; mode < 9; mode++) {
        reset(); f.value = "2048";
        switch (mode) {
        case 0: f.interfaces[0].ifa_name = (char *)"wlan0"; break;
        case 1: f.interfaces[0].ifa_name = (char *)"ppp1"; break;
        case 2: f.interfaces[0].ifa_name = NULL; break;
        case 3: f.interfaces[0].ifa_addr = NULL; break;
        case 4: f.addresses[0].sin_family = AF_INET6; break;
        case 5: f.interfaces[0].ifa_flags = IFF_UP; break;
        case 6: f.interfaces[0].ifa_flags = IFF_RUNNING; break;
        case 7: f.addresses[0].sin_addr.s_addr = 0; break;
        case 8: f.head = NULL; break;
        }
        assert_disabled(); assert(f.if_calls == 1 && f.free_calls == 1 && !f.logs);
    }
    reset(); f.value = "2048"; f.if_rc = -1;
    assert_disabled(); assert(f.if_calls == 1 && !f.free_calls && !f.logs);

    reset(); f.value = "2048"; f.set_rc = -1;
    apply(); assert(f.set_calls == 1 && !f.get_calls && f.free_calls == 1);
    assert(f.warnings == 1 && !f.infos);
    assert(strstr(f.log, "actual=-1 set_rc=-1"));
    assert(strstr(f.log, "get_rc=-1 get_errno=0"));
    char expected_error[64];
    (void)snprintf(expected_error, sizeof(expected_error), "set_errno=%d", ENOBUFS);
    assert(strstr(f.log, expected_error));

    reset(); f.value = "2048"; f.get_rc = -1;
    apply(); assert(f.set_calls == 1 && f.get_calls == 1 && f.free_calls == 1);
    assert(f.warnings == 1 && !f.infos);
    assert(strstr(f.log, "actual=-1 set_rc=0"));
    assert(strstr(f.log, "get_rc=-1"));
    (void)snprintf(expected_error, sizeof(expected_error), "get_errno=%d", ENOTSOCK);
    assert(strstr(f.log, expected_error));

    reset(); f.value = "2048"; f.actual_size = sizeof(int) - 1;
    apply(); assert(f.warnings == 1 && !f.infos);
    assert(strstr(f.log, "actual=-1"));

    reset(); f.value = "2048"; f.actual = 1024;
    apply(); assert(f.warnings == 1 && !f.infos);
    assert(strstr(f.log, "requested=2048 actual=1024"));

    /* Direct getter also proves exact selection, without a socket mutation. */
    reset(); f.value = "4096";
    assert(zhaoxi_ppp_rx_window() == 4096);
    assert(f.config_calls == 1 && f.if_calls == 1 && f.free_calls == 1);
    assert(!f.set_calls && !f.get_calls && !f.logs); checks++;

    printf("PPP_RX_WINDOW: %u checks passed; real interfaces=0 sockets=0\n", checks);
    return 0;
}

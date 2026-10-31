/* SPDX-License-Identifier: Apache-2.0 */
/* Isolated diagnostic of the real NuttX resolver. No external DNS service,
 * Wi-Fi or model is involved; synthetic answers cannot prove Internet access. */
#include "dns_diag.h"
#include "core/message_bus.h"
#include <nuttx/net/dns.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

struct dns_snapshot {
    struct sockaddr_storage addresses[CONFIG_NETDB_DNSSERVER_NAMESERVERS];
    socklen_t lengths[CONFIG_NETDB_DNSSERVER_NAMESERVERS];
    int count;
};

static int remember_server(void *arg, struct sockaddr *address, socklen_t length)
{
    struct dns_snapshot *saved = arg;
    if (saved->count >= CONFIG_NETDB_DNSSERVER_NAMESERVERS ||
        length > sizeof(struct sockaddr_storage)) return -EOVERFLOW;
    memcpy(&saved->addresses[saved->count], address, length);
    saved->lengths[saved->count++] = length;
    return 0;
}

int zhaoxi_dns_info(void)
{
    struct dns_snapshot saved = {0};
    int result = dns_foreach_nameserver(remember_server, &saved);
    printf("DNS_INFO count=%d rc=%d recv_timeout_s=%d retries=%d\n", saved.count,
           result, CONFIG_NETDB_DNSCLIENT_RECV_TIMEOUT, CONFIG_NETDB_DNSCLIENT_RETRIES);
    for (int i = 0; i < saved.count; i++) {
        char text[INET6_ADDRSTRLEN] = "?";
        struct sockaddr *address = (struct sockaddr *)&saved.addresses[i];
        unsigned port = 0;
        if (address->sa_family == AF_INET) {
            struct sockaddr_in *ip = (struct sockaddr_in *)address;
            inet_ntop(AF_INET, &ip->sin_addr, text, sizeof(text)); port = ntohs(ip->sin_port);
        } else if (address->sa_family == AF_INET6) {
            struct sockaddr_in6 *ip = (struct sockaddr_in6 *)address;
            inet_ntop(AF_INET6, &ip->sin6_addr, text, sizeof(text)); port = ntohs(ip->sin6_port);
        }
        printf("DNS_SERVER index=%d family=%d address=%s port=%u\n", i, address->sa_family, text, port);
    }
    return result;
}

struct fixture {
    int fd;
    atomic_bool stop;
    atomic_uint received;
};

static void u16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static size_t name_wire(uint8_t *out, const char *name)
{
    size_t size = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        out[size++] = n; memcpy(out + size, name, n); size += n;
        name += n; if (*name == '.') name++;
    }
    out[size++] = 0;
    return size;
}

int zhaoxi_dns_probe(const char *server, const char *hostname)
{
    /* A separate connected UDP socket shows whether a wire response arrives
     * before the system resolver's parsing/error-code conversion. This does
     * not alter nameservers, routes or the station configuration. */
    if (!server || !hostname || !hostname[0] || strlen(hostname) > 253) return -EINVAL;
    const char *label = hostname;
    for (const char *p = hostname; ; p++) {
        unsigned char c = *p;
        if (!c || c == '.') {
            if (p == label || p - label > 63) return -EINVAL;
            if (!c) break;
            label = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_')) return -EINVAL;
    }
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(53)};
    if (inet_pton(AF_INET, server, &remote.sin_addr) != 1) return -EINVAL;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -errno;
    struct timeval timeout = {.tv_sec = 5};
    int result = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, (struct sockaddr *)&remote, sizeof(remote))) {
        result = -errno;
        printf("DNS_PROBE server=%s setup_rc=%d\n", server, result);
        close(fd); return result;
    }
    uint8_t packet[512] = {0};
    struct timespec started, ended; clock_gettime(CLOCK_MONOTONIC, &started);
    uint16_t id = (uint16_t)(started.tv_nsec ^ started.tv_sec);
    u16(packet, id); u16(packet + 2, 0x0100); u16(packet + 4, 1);
    size_t size = 12 + name_wire(packet + 12, hostname);
    u16(packet + size, 1); u16(packet + size + 2, 1); size += 4;
    int sent = send(fd, packet, size, 0);
    int received = -1;
    if (sent < 0) result = -errno;
    else if (sent != (int)size) result = -EIO;
    else {
        received = recv(fd, packet, sizeof(packet), 0);
        if (received < 0) result = -errno;
        else if (received < 12) result = -EIO;
    }
    clock_gettime(CLOCK_MONOTONIC, &ended);
    long elapsed = (ended.tv_sec - started.tv_sec) * 1000L +
                   (ended.tv_nsec - started.tv_nsec) / 1000000L;
    printf("DNS_PROBE server=%s host=%s sent=%d received=%d rc=%d elapsed_ms=%ld\n",
           server, hostname, sent, received, result, elapsed);
    if (!result) {
        unsigned reply_id = ((unsigned)packet[0] << 8) | packet[1];
        bool valid = reply_id == id && (packet[2] & 0x80);
        printf("DNS_PROBE_RESPONSE id_matches=%d qr=%d rcode=%u truncated=%d answers=%u\n",
               reply_id == id, !!(packet[2] & 0x80), packet[3] & 15, !!(packet[2] & 2),
               ((unsigned)packet[6] << 8) | packet[7]);
        if (!valid) result = -EPROTO;
    }
    close(fd);
    return result;
}

static void *fixture_server(void *arg)
{
    struct fixture *server = arg;
    while (!atomic_load(&server->stop)) {
        uint8_t packet[512]; struct sockaddr_in peer; socklen_t length = sizeof(peer);
        int count = recvfrom(server->fd, packet, sizeof(packet), 0, (struct sockaddr *)&peer, &length);
        if (count < 12) continue;
        if (packet[4] || packet[5] != 1 || packet[2] & 0x80) continue;
        size_t at = 12, used = 0; char name[128] = {0}; bool valid = false;
        while (at < (size_t)count) {
            unsigned n = packet[at++];
            if (!n) { valid = true; break; }
            if (n > 63 || at + n > (size_t)count || used + n + 1 >= sizeof(name)) break;
            if (used) name[used++] = '.';
            memcpy(name + used, packet + at, n); used += n; at += n;
        }
        if (!valid || at + 4 > (size_t)count) continue;
        unsigned type = ((unsigned)packet[at] << 8) | packet[at + 1];
        if (type != 1 || packet[at + 2] || packet[at + 3] != 1) continue;
        at += 4; atomic_fetch_add(&server->received, 1);
        packet[2] = 0x81; packet[3] = 0x80;
        memset(packet + 6, 0, 6);
        bool cname = !strcmp(name, "cname.dns-selftest.invalid");
        bool direct = !strcmp(name, "a.dns-selftest.invalid");
        if (!cname && !direct) {
            packet[3] = 0x83; /* controlled NXDOMAIN */
        } else {
            u16(packet + 6, cname ? 2 : 1);
            if (cname) {
                packet[at++] = 0xc0; packet[at++] = 0x0c;
                u16(packet + at, 5); u16(packet + at + 2, 1);
                memset(packet + at + 4, 0, 4); at += 8;
                size_t size = name_wire(packet + at + 2, "a.dns-selftest.invalid");
                u16(packet + at, size); at += 2 + size;
                at += name_wire(packet + at, "a.dns-selftest.invalid");
            } else {
                packet[at++] = 0xc0; packet[at++] = 0x0c;
            }
            u16(packet + at, 1); u16(packet + at + 2, 1);
            memset(packet + at + 4, 0, 4); u16(packet + at + 8, 4); at += 10;
            packet[at++] = 192; packet[at++] = 0; packet[at++] = 2; packet[at++] = 123;
        }
        sendto(server->fd, packet, at, 0, (struct sockaddr *)&peer, length);
    }
    return NULL;
}

static int lookup_case(const char *name, bool expect_answer, struct fixture *server)
{
    struct addrinfo *answer = NULL;
    struct addrinfo hint = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    unsigned before = atomic_load(&server->received);
    int result = getaddrinfo(name, NULL, &hint, &answer);
    char address[INET_ADDRSTRLEN] = "none";
    if (!result && answer && answer->ai_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)answer->ai_addr)->sin_addr, address, sizeof(address));
    bool received = atomic_load(&server->received) > before;
    bool passed = received && (expect_answer ? !result && !strcmp(address, "192.0.2.123") : result != 0);
    printf("DNS_SELFTEST name=%s rc=%d address=%s query_received=%d passed=%d\n",
           name, result, address, received, passed);
    if (answer) freeaddrinfo(answer);
    return passed ? 0 : -EIO;
}

int zhaoxi_dns_selftest(void)
{
    /* Avoid replacing a server while the Agent or its network watcher is
     * using/updating DNS. The caller must wait for real Agent shutdown. */
    if (message_bus_is_ready()) { puts("Stop Agent before isolated DNS self-test."); return -EBUSY; }
    struct dns_snapshot saved = {0};
    int result = dns_foreach_nameserver(remember_server, &saved);
    if (result < 0) return result;
    struct fixture server = {.fd = -1};
    atomic_init(&server.stop, false); atomic_init(&server.received, 0);
    server.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server.fd < 0) return -errno;
    struct sockaddr_in local = {.sin_family = AF_INET};
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    struct timeval timeout = {.tv_sec = 1};
    if (setsockopt(server.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        bind(server.fd, (struct sockaddr *)&local, sizeof(local))) {
        result = -errno; close(server.fd); return result;
    }
    socklen_t length = sizeof(local);
    if (getsockname(server.fd, (struct sockaddr *)&local, &length)) {
        result = -errno; close(server.fd); return result;
    }
    pthread_t worker;
    result = pthread_create(&worker, NULL, fixture_server, &server);
    if (result) { close(server.fd); return -result; }
    result = dns_default_nameserver();
    if (!result) result = dns_add_nameserver((struct sockaddr *)&local, sizeof(local));
    if (!result) result = dns_set_queryfamily(AF_INET);
    if (!result) result = lookup_case("a.dns-selftest.invalid", true, &server);
    if (!result) result = lookup_case("cname.dns-selftest.invalid", true, &server);
    if (!result) result = lookup_case("missing.dns-selftest.invalid", false, &server);
    atomic_store(&server.stop, true); pthread_join(worker, NULL); close(server.fd);
    int restored = dns_default_nameserver();
    for (int i = 0; i < saved.count; i++) {
        int rc = dns_add_nameserver((struct sockaddr *)&saved.addresses[i], saved.lengths[i]);
        if (rc) restored = rc;
    }
    printf("DNS_SELFTEST finished rc=%d restore_rc=%d original_servers=%d synthetic_only=1\n",
           result, restored, saved.count);
    return result ? result : restored;
}

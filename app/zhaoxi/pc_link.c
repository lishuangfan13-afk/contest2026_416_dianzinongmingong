/* SPDX-License-Identifier: Apache-2.0 */
/* Bounded PPP proof: native board DNS/TLS over the existing USB-UART cable.
 * The computer forwards IP; it does not replace the board's model client. */
#include "pc_link.h"
#include "console_lease.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "cJSON.h"
#include "core/message_bus.h"
#include "infra/task_lifecycle.h"
#include "voice/voice_channel.h"
#include <nuttx/config.h>
#include <nuttx/net/dns.h>
#include <netutils/netlib.h>
#include <netutils/pppd.h>
#include <net/route.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

static atomic_bool stopping = ATOMIC_VAR_INIT(true);
static atomic_bool worker_alive = ATOMIC_VAR_INIT(false);
static atomic_bool ipv4_ready = ATOMIC_VAR_INIT(false);
static atomic_bool link_ready = ATOMIC_VAR_INIT(false);
extern int zhaoxi_pc_control(void);
extern int zhaoxi_pc_boot_control(void);
bool zhaoxi_pc_link_stopping(void) { return atomic_load(&stopping); }
bool zhaoxi_pc_link_alive(void) { return atomic_load(&worker_alive); }
bool zhaoxi_pc_link_ready(void) { return atomic_load(&link_ready); }
void zhaoxi_pc_link_request_stop(void) { atomic_store(&stopping,true); }
void zhaoxi_pc_link_ipv4_ready(void) { atomic_store(&ipv4_ready, true); }
static void *run_ppp(void *arg)
{
    pppd(arg);
    atomic_store(&worker_alive, false);
    return NULL;
}

struct saved_dns {
    struct sockaddr_storage server[CONFIG_NETDB_DNSSERVER_NAMESERVERS];
    socklen_t length[CONFIG_NETDB_DNSSERVER_NAMESERVERS];
    int count;
};
static int save_dns(void *arg, struct sockaddr *address, socklen_t length)
{
    struct saved_dns *saved = arg;
    if (saved->count >= CONFIG_NETDB_DNSSERVER_NAMESERVERS || length > sizeof(saved->server[0])) return -EOVERFLOW;
    memcpy(&saved->server[saved->count], address, length);
    saved->length[saved->count++] = length;
    return 0;
}

static int route_half(int fd, unsigned first, bool add)
{
    struct sockaddr_in target = {.sin_family = AF_INET};
    struct sockaddr_in mask = {.sin_family = AF_INET};
    struct sockaddr_in gateway = {.sin_family = AF_INET};
    target.sin_addr.s_addr = htonl(first ? 0x80000000 : 0);
    mask.sin_addr.s_addr = htonl(0x80000000);
    inet_pton(AF_INET, "10.254.42.1", &gateway.sin_addr);
    return add ? addroute(fd, &target, &mask, &gateway, sizeof(target)) :
                 delroute(fd, &target, &mask, sizeof(target));
}

/* The command server has already sent and drained the quit reply. Keep
 * PPP/routes/DNS alive while this session's Agent exits its real workers. */
static int stop_session_agent(uint64_t owner, unsigned budget_ms, cJSON *report)
{
    if (!owner) return 0; /* A borrowed external instance is not ours to stop. */
    bool requested = agent_instance_shutdown_owned(owner);
    int result = agent_instance_wait_owned(owner, budget_ms);
    cJSON_AddBoolToObject(report, "agent_owned_shutdown_requested", requested);
    cJSON_AddNumberToObject(report, "agent_shutdown_wait_rc", result);
    cJSON_AddBoolToObject(report, "agent_owned_instance_exited", result == 0);
    return result;
}

static int pc_link_run(unsigned seconds,bool serving,bool persistent)
{
    if (seconds < 45 || seconds > 600) return -EINVAL;
    bool expected = true;
    if (!atomic_compare_exchange_strong(&stopping, &expected, false)) return -EBUSY;
    atomic_store(&link_ready,false);
    uint64_t session_owner = 0;
    struct saved_dns saved = {0};
    if (dns_foreach_nameserver(save_dns, &saved) < 0) { atomic_store(&stopping, true); return -EIO; }
    struct pppd_settings_s settings = {0};
    strcpy(settings.ttyname, "/dev/console");
    cJSON *report = cJSON_CreateObject();
    if (!report) { atomic_store(&stopping, true); return -ENOMEM; }
    cJSON_AddStringToObject(report, "transport", "PPP over USB2UART; computer forwards IP");
    cJSON_AddBoolToObject(report, "board_native_tls", true);
    cJSON_AddNumberToObject(report, "negotiation_budget_seconds", seconds);
    cJSON_AddBoolToObject(report, "serving", serving);
    int lease_rc=zhaoxi_console_claim();
    if(lease_rc){cJSON_Delete(report);atomic_store(&stopping,true);return lease_rc;}
    int tty = open("/dev/console", O_RDWR | O_NONBLOCK);
    struct termios previous = {0};
    bool raw_saved = tty >= 0 && ioctl(tty, TCGETS, (unsigned long)&previous) == 0;
    if (!raw_saved) { if (tty >= 0) close(tty); zhaoxi_console_release();cJSON_Delete(report); atomic_store(&stopping, true); return -ENOTTY; }
    struct termios raw = previous;
    raw.c_iflag = 0; raw.c_oflag = 0; raw.c_lflag = 0;
    cJSON_AddNumberToObject(report, "previous_tty_lflag", previous.c_lflag);
    cJSON_AddNumberToObject(report, "previous_tty_iflag", previous.c_iflag);
    if (ioctl(tty, TCSETS, (unsigned long)&raw)) {
        int error=-errno;close(tty);zhaoxi_console_release();cJSON_Delete(report);atomic_store(&stopping,true);return error;
    }
    printf("PC_LINK starting %u seconds; console temporarily carries PPP.\n", seconds);
    fflush(stdout);
    int old_mask = setlogmask(LOG_UPTO(LOG_WARNING));
    pthread_attr_t attr;
    pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8192);
    pthread_t worker;
    atomic_store(&ipv4_ready, false);
    atomic_store(&worker_alive, true);
    int result = pthread_create(&worker, &attr, run_ppp, &settings);
    pthread_attr_destroy(&attr);
    int fd = -1; bool route0 = false, route1 = false, dns_changed = false;
    bool cleanup_ok = true;
    if (result) { atomic_store(&worker_alive, false); result = -result; goto finish; }
    struct in_addr address = {0};
    unsigned waited = 0;
    for (; waited < seconds - 30; waited++) {
        if (!zhaoxi_pc_link_alive()) { result = -ENETDOWN; goto stop; }
        if (atomic_load(&ipv4_ready) &&
            !netlib_get_ipv4addr("ppp0", &address) && address.s_addr) break;
        sleep(1);
    }
    char text[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, &address, text, sizeof(text));
    cJSON_AddStringToObject(report, "ppp_ipv4", text);
    cJSON_AddNumberToObject(report, "negotiation_seconds", waited);
    cJSON_AddBoolToObject(report, "ipcp_ipv4_ready", atomic_load(&ipv4_ready));
    if (!atomic_load(&ipv4_ready) || strcmp(text, "10.254.42.2")) {
        result = -ENETUNREACH; goto stop;
    }
    struct in_addr mask, peer;
    inet_pton(AF_INET, "255.255.255.252", &mask);
    inet_pton(AF_INET, "10.254.42.1", &peer);
    if (netlib_set_ipv4netmask("ppp0", &mask) || netlib_set_dripv4addr("ppp0", &peer) || netlib_set_mtu("ppp0", 512)) {
        result = -errno; goto stop;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { result = -errno; goto stop; }
    route0 = route_half(fd, 0, true) == 0;
    route1 = route_half(fd, 1, true) == 0;
    cJSON_AddBoolToObject(report, "routes_installed", route0 && route1);
    if (!route0 || !route1) { result = -errno; goto stop; }
    struct sockaddr_in dns = {.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, "223.5.5.5", &dns.sin_addr);
    result = dns_default_nameserver();
    dns_changed = true;
    if (!result) result = dns_add_nameserver((struct sockaddr *)&dns, sizeof(dns));
    if (result) goto stop;
    dns_set_queryfamily(AF_INET);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *answer = NULL;
    /* EAI_AGAIN is temporary. A failed startup probe must not discard an
     * otherwise healthy PPP session after a single resolver attempt.
     * NuttX's resolver has its own finite per-query socket retry bound.
     */
    int gai = EAI_AGAIN;
    cJSON *dns_attempts = cJSON_AddArrayToObject(report, "external_dns_attempts");
    for (unsigned attempt = 1; attempt <= 3; attempt++) {
        if (!zhaoxi_pc_link_alive()) { result = -ENETDOWN; goto stop; }
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        errno = 0;
        gai = getaddrinfo("token-plan-cn.xiaomimimo.com", NULL, &hints, &answer);
        int dns_errno = errno;
        clock_gettime(CLOCK_MONOTONIC, &end);
        cJSON *record = cJSON_CreateObject();
        if (record && dns_attempts) {
            cJSON_AddNumberToObject(record, "attempt", attempt);
            cJSON_AddNumberToObject(record, "gai", gai);
            cJSON_AddNumberToObject(record, "errno", dns_errno);
            cJSON_AddNumberToObject(record, "elapsed_ms",
                (end.tv_sec-begin.tv_sec)*1000 + (end.tv_nsec-begin.tv_nsec)/1000000);
            cJSON_AddItemToArray(dns_attempts, record);
        } else cJSON_Delete(record);
        if (!gai && answer) break;
        if (answer) { freeaddrinfo(answer); answer = NULL; }
        if (gai != EAI_AGAIN || attempt == 3) break;
        sleep(attempt); /* 1 second, then 2 seconds; no unbounded retry. */
    }
    cJSON_AddNumberToObject(report, "external_dns_rc", gai);
    if (gai || !answer) { result = -EHOSTUNREACH; goto stop; }
    inet_ntop(AF_INET, &((struct sockaddr_in *)answer->ai_addr)->sin_addr, text, sizeof(text));
    cJSON_AddStringToObject(report, "external_dns_ipv4", text);
    freeaddrinfo(answer);
    if (serving) {
        if (!message_bus_is_ready()) {
            /* A stopping external instance is still alive even though its
             * bus is closed; never launch over it or claim its ownership. */
            if (agent_instance_is_active()) { result = -EBUSY; goto stop; }
            session_owner = agent_session_owner_new();
            if (!session_owner) { result = -EBUSY; goto stop; }
            char launch[96];
            snprintf(launch, sizeof(launch), "ai_agent --headless --pc-owner %llu &",
                     (unsigned long long)session_owner);
            if (system(launch)) { result = -EIO; goto stop; }
            for (unsigned i = 0; i < 20 && !message_bus_is_ready(); i++) usleep(500000);
            if (!message_bus_is_ready()) { result = -ETIMEDOUT; goto stop; }
        }
        cJSON_AddBoolToObject(report, "agent_started_by_link",
                              agent_instance_is_owned(session_owner));
        atomic_store(&link_ready,true);
        result = persistent?zhaoxi_pc_boot_control():zhaoxi_pc_control(); goto stop;
    }
    char key[128] = {0};
    if (claw_config_get("api_key", key, sizeof(key)) || !key[0]) { result = -EACCES; goto stop; }
    const char *host = !strncmp(key, "tp-", 3) || !strncmp(key, "ttp-", 4) ?
        "token-plan-cn.xiaomimimo.com" : "api.xiaomimimo.com";
    vela_header_t headers[] = {{"api-key", key}, {NULL, NULL}};
    char *response = calloc(1, 16384);
    if (!response) { memset(key, 0, sizeof(key)); result = -ENOMEM; goto stop; }
    int http = vela_https_post_json(host, "443", "/v1/chat/completions", headers,
        "{\"model\":\"mimo-v2.6-flash\",\"messages\":[{\"role\":\"user\",\"content\":\"Reply only: PPP network works\"}],\"max_tokens\":64,\"thinking\":{\"type\":\"disabled\"}}",
        response, 16384);
    memset(key, 0, sizeof(key));
    cJSON_AddNumberToObject(report, "board_https_status", http);
    cJSON *body = http == 200 ? cJSON_Parse(response) : NULL;
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(
        cJSON_GetArrayItem(cJSON_GetObjectItem(body, "choices"), 0), "message"), "content"));
    if (content) cJSON_AddStringToObject(report, "model_answer", content);
    result = http == 200 && content ? 0 : -EIO;
    cJSON_Delete(body); free(response);
stop:
    atomic_store(&link_ready,false);
    if (serving) voice_channel_auto_stop();
    /* Do not remove a healthy PPP link first: net_watch could enter a
     * synchronous BES reconnect and never reach the shutdown flag again. */
    int shutdown_result = stop_session_agent(session_owner, 45000, report);
    if (shutdown_result) {cleanup_ok=false;if(!result)result=shutdown_result;}
    atomic_store(&stopping, true);
    int joined = pthread_join(worker, NULL);
    cJSON_AddNumberToObject(report, "ppp_join_rc", joined);
    if (joined) cleanup_ok=false;
    if (route0) {int removed=route_half(fd,0,false);cJSON_AddNumberToObject(report,"route0_remove_rc",removed);if(removed)cleanup_ok=false;}
    if (route1) {int removed=route_half(fd,1,false);cJSON_AddNumberToObject(report,"route1_remove_rc",removed);if(removed)cleanup_ok=false;}
    if (fd >= 0 && close(fd)) cleanup_ok=false;
    if (dns_changed) {
        int restored = dns_default_nameserver();
        for (int i = 0; i < saved.count; i++) {
            int rc = dns_add_nameserver((struct sockaddr *)&saved.server[i], saved.length[i]);
            if (rc) restored = rc;
        }
        cJSON_AddNumberToObject(report, "dns_restore_rc", restored);
        if(restored)cleanup_ok=false;
    }
finish:
    atomic_store(&ipv4_ready, false);
    atomic_store(&stopping, true); setlogmask(old_mask);
    int restored_tty = ioctl(tty, TCSETS, (unsigned long)&previous);
    cJSON_AddNumberToObject(report, "tty_restore_rc", restored_tty);
    int closed_tty=close(tty);
    if(restored_tty || closed_tty)cleanup_ok=false;
    /* Never re-enable shell reads over a live PPP worker or raw console. */
    int lease_release_rc=(!restored_tty && !atomic_load(&worker_alive))?zhaoxi_console_release():-EBUSY;
    if(lease_release_rc)cleanup_ok=false;
    cJSON_AddNumberToObject(report,"console_release_rc",lease_release_rc);
    cJSON_AddBoolToObject(report,"cleanup_confirmed",cleanup_ok);
    cJSON_AddNumberToObject(report, "result", result);
    char *output = cJSON_PrintUnformatted(report);
    if (output) {
        FILE *file = fopen("/tmp/pc_link_result.json", "w");
        if (file) { fputs(output, file); fclose(file); }
        free(output);
    }
    cJSON_Delete(report);
    printf("\nPC_LINK_CLEANUP confirmed=%d\n",cleanup_ok);
    printf("PC_LINK finished rc=%d; result /tmp/pc_link_result.json; console restored.\n", result);
    fflush(stdout);
    return result;
}
int zhaoxi_pc_link(unsigned seconds)
{return pc_link_run(seconds?seconds:120,seconds==0,false);}
int zhaoxi_pc_link_boot(void)
{return pc_link_run(60,true,true);}

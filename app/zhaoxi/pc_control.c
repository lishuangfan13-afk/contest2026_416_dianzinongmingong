/* SPDX-License-Identifier: Apache-2.0 */
/* Command channel restricted to the directly attached PPP computer. */
#include "cJSON.h"
#include "tools/tool_registry.h"
#include "pc_link.h"
#include "journal.h"
#include "morning.h"
#include "voice/voice_channel.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <netutils/netlib.h>
#include <net/if.h>

extern int zhaoxi_main(int argc, char **argv);

static cJSON *dispatch(cJSON *request, bool *ending)
{
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return NULL;
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(request, "action"));
    int rc = -EINVAL;
    if (action && !strcmp(action, "ping")) {
        rc = 0;
        cJSON_AddStringToObject(reply, "transport", "native BES PPP via computer");
    } else if (action && !strcmp(action, "quit")) {
        voice_channel_auto_stop();
        rc = 0; *ending = true;
        cJSON_AddBoolToObject(reply, "ending", true);
    } else if(action && !strcmp(action,"sync-time")) {
        cJSON *epoch=cJSON_GetObjectItem(request,"epoch");
        if(cJSON_IsNumber(epoch) && epoch->valuedouble>=1735689600LL && epoch->valuedouble<=4102444799LL &&
           epoch->valuedouble==(double)(int64_t)epoch->valuedouble) {
            rc=zhaoxi_morning_sync_time((int64_t)epoch->valuedouble,"PC");
            if(!rc)cJSON_AddNumberToObject(reply,"epoch",(double)time(NULL));
        }
    } else if(action && (!strcmp(action,"profile-get") || !strcmp(action,"profile-set"))) {
        if(!strcmp(action,"profile-set"))rc=zhaoxi_morning_set_profile(cJSON_GetStringValue(cJSON_GetObjectItem(request,"text")));
        else rc=0;
        if(!rc){char profile[MORNING_PROFILE_MAX+1];rc=morning_profile_get(profile,sizeof(profile));if(!rc)cJSON_AddStringToObject(reply,"profile",profile);}
    } else if(action && !strcmp(action,"news-status")) {
        char text[1400];rc=zhaoxi_morning_status(text,sizeof(text));
        cJSON *v=!rc?cJSON_Parse(text):NULL;if(v)cJSON_AddItemToObject(reply,"news",v);
    } else if(action && !strcmp(action,"news-get")) {
        struct morning_batch *batch=calloc(1,sizeof(*batch));char *text=malloc(MORNING_JSON_MAX),today[11];
        rc=(!batch || !text)?-ENOMEM:morning_day((int64_t)time(NULL),today,NULL);
        if(!rc)rc=morning_batch_load(today,batch);
        if(!rc)rc=morning_batch_render(batch,text,MORNING_JSON_MAX);
        cJSON *v=!rc?cJSON_Parse(text):NULL;if(v)cJSON_AddItemToObject(reply,"news",v);
        free(batch);free(text);
    } else if (action && !strcmp(action, "auto-status")) {
        char status[1024] = {0};
        rc = voice_channel_auto_status(status, sizeof(status));
        cJSON *state = !rc ? cJSON_Parse(status) : NULL;
        if (state) cJSON_AddItemToObject(reply, "auto_voice", state);
    } else if (action && (!strcmp(action, "notes") || !strcmp(action, "tasks") ||
                          !strcmp(action, "time") || !strcmp(action, "reminders"))) {
        char *out = calloc(1, 16384);
        bool reading = !strcmp(action, "notes") || !strcmp(action, "tasks");
        const char *tool = reading ? "read_file" :
                           !strcmp(action, "time") ? "get_current_time" : "cron_list";
        char input[160] = "{}";
        if (reading) {
            char path[96] = "/emmc/zhaoxi/TASKS.md";
            if (!strcmp(action, "notes")) {
                time_t now = time(NULL);
                if (zhaoxi_note_path(now, path, sizeof(path))) {
                    free(out); cJSON_AddNumberToObject(reply, "rc", -EINVAL); return reply;
                }
            }
            snprintf(input, sizeof(input), "{\"path\":\"%s\"}", path);
            cJSON_AddStringToObject(reply, "path", path);
        }
        if (out) {
            rc = tool_registry_execute(tool, input, out, 16384);
            cJSON_AddStringToObject(reply, "tool_output", out); free(out);
        } else rc = -ENOMEM;
    } else if (action && !strcmp(action, "command")) {
        cJSON *args = cJSON_GetObjectItem(request, "args");
        int count = cJSON_GetArraySize(args); size_t total = 0;
        char *argv[18] = {"zhaoxi"}; bool valid = cJSON_IsArray(args) && count > 0 && count < 17;
        for (int i = 0; valid && i < count; i++) {
            argv[i + 1] = (char *)cJSON_GetStringValue(cJSON_GetArrayItem(args, i));
            if (!argv[i + 1] || !argv[i + 1][0]) { valid = false; break; }
            total += strlen(argv[i + 1]);
            if (total > 1024) valid = false;
        }
        int op = valid && !strcmp(argv[1], "--text") ? 2 : 1;
        const char *allowed[] = {"ask", "note", "todo", "remind", "drink", "notify", "listen", "auto", "say", "status", "test-speaker", "profile", "news"};
        bool permitted = false;
        if (valid && op <= count) {
            for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
                if (!strcmp(argv[op], allowed[i])) permitted = true;
        }
        if (permitted) rc = zhaoxi_main(count + 1, argv);
        cJSON_AddBoolToObject(reply, "command_return_only", true);
    }
    cJSON_AddNumberToObject(reply, "rc", rc);
    return reply;
}

/* Each complete request/reply gets five seconds, regardless of byte progress.
 * Nonblocking I/O keeps readiness races from exceeding the 100 ms stop slice. */
#define PC_CONTROL_IO_MS 5000
#define PC_CONTROL_POLL_MS 100

static int64_t pc_control_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int pc_control_link_status(void)
{
    if (zhaoxi_pc_link_stopping()) return -ECANCELED;
    if (!zhaoxi_pc_link_alive()) return -ENETDOWN;
    return 0;
}

static int pc_control_wait(int fd, short events, int64_t deadline)
{
    for (;;) {
        int rc = pc_control_link_status();
        if (rc) return rc;
        int64_t now = pc_control_now_ms();
        if (now < 0) return -EIO;
        if (now >= deadline) return -ETIMEDOUT;
        int timeout = (int)(deadline - now);
        if (timeout > PC_CONTROL_POLL_MS) timeout = PC_CONTROL_POLL_MS;
        struct pollfd waiter = {.fd = fd, .events = events};
        rc = poll(&waiter, 1, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (!rc) continue;
        /* poll may have consumed the remaining budget or observed a stop. */
        rc = pc_control_link_status();
        if (rc) return rc;
        now = pc_control_now_ms();
        if (now < 0) return -EIO;
        if (now >= deadline) return -ETIMEDOUT;
        if (waiter.revents & POLLNVAL) return -EBADF;
        if (waiter.revents & events) return 0;
        if (waiter.revents & (POLLERR | POLLHUP)) return -ECONNRESET;
    }
}

static int pc_control_receive(int fd, char *input, size_t capacity)
{
    int64_t now = pc_control_now_ms();
    if (now < 0) return -EIO;
    int64_t deadline = now + PC_CONTROL_IO_MS;
    size_t used = 0;
    while (used < capacity - 1) {
        int rc = pc_control_wait(fd, POLLIN, deadline);
        if (rc) return rc;
        ssize_t got = recv(fd, input + used, capacity - 1 - used, 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -errno;
        }
        if (!got) return -ECONNRESET;
        used += (size_t)got;
        char *newline = memchr(input, '\n', used);
        if (newline) { *newline = 0; return 0; }
    }
    return -EMSGSIZE;
}

static int pc_control_send(int fd, const char *output)
{
    int64_t now = pc_control_now_ms();
    if (now < 0) return -EIO;
    int64_t deadline = now + PC_CONTROL_IO_MS;
    size_t size = strlen(output), sent = 0;
    /* Include the framing newline in the same absolute send deadline. */
    while (sent <= size) {
        int rc = pc_control_wait(fd, POLLOUT, deadline);
        if (rc) return rc;
        ssize_t n = sent < size ? send(fd, output + sent, size - sent, 0) :
                                 send(fd, "\n", 1, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -errno;
        }
        if (!n) return -ECONNRESET;
        sent += (size_t)n;
    }
    return 0;
}

static int pc_control_run(bool persistent)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -errno;
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(8765)};
    inet_pton(AF_INET, "10.254.42.2", &local.sin_addr);
    int reuse = 1; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(listener, (struct sockaddr *)&local, sizeof(local)) || listen(listener, 2)) {
        int error = -errno; close(listener); return error;
    }
    puts("PC_LINK ready; wired command channel available."); fflush(stdout);
    bool ending = false;
    struct timespec started, now; clock_gettime(CLOCK_MONOTONIC, &started);
    while (!ending) {
        if(zhaoxi_pc_link_stopping()){close(listener);return -ECANCELED;}
        uint8_t flags = 0;
        if (!zhaoxi_pc_link_alive() || netlib_getifstatus("ppp0", &flags) ||
            (flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) {
            close(listener); return -ENETDOWN;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!persistent && now.tv_sec - started.tv_sec >= 3600) break;
        struct pollfd waiter = {.fd = listener, .events = POLLIN};
        if (poll(&waiter, 1, PC_CONTROL_POLL_MS) <= 0) continue;
        struct sockaddr_in peer; socklen_t length = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &length);
        if (fd < 0) continue;
        struct in_addr computer; inet_pton(AF_INET, "10.254.42.1", &computer);
        if (peer.sin_addr.s_addr != computer.s_addr) { close(fd); continue; }
        int fd_flags = fcntl(fd, F_GETFL, 0);
        if (fd_flags < 0 || fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) < 0) {
            close(fd); continue;
        }
        char input[2048];
        int io_rc = pc_control_receive(fd, input, sizeof(input));
        if (!io_rc) io_rc = pc_control_link_status();
        cJSON *request = !io_rc ? cJSON_Parse(input) : NULL;
        cJSON *reply = request ? dispatch(request, &ending) : NULL;
        char *output = reply ? cJSON_PrintUnformatted(reply) : NULL;
        bool reply_sent = false;
        if (output) {
            io_rc = pc_control_send(fd, output);
            reply_sent = !io_rc;
            free(output);
        }
        cJSON_Delete(reply); cJSON_Delete(request); close(fd);
        if (io_rc == -ECANCELED || io_rc == -ENETDOWN) {
            close(listener); return io_rc;
        }
        /* Let the complete quit reply and TCP close reach the attached computer
         * before the caller removes the interface carrying that connection. */
        if (ending && reply_sent) usleep(250000);
    }
    close(listener);
    return 0;
}
int zhaoxi_pc_control(void){return pc_control_run(false);}
int zhaoxi_pc_boot_control(void){return pc_control_run(true);}

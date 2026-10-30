/* SPDX-License-Identifier: Apache-2.0
 * Screen-free product entry. The AI Agent owns tools, storage and speech.
 */

#include "core/message_bus.h"
#include "voice/voice_channel.h"
#include "agent_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_ZHAOXI_BES_AUDIO
#include "voice/audio_playback.h"
#include "tools/tool_registry.h"
#include <netutils/netlib.h>
#include <stdint.h>
#include <time.h>
#ifdef CONFIG_NETDB_DNSCLIENT
#include <arpa/inet.h>
#include <netinet/in.h>
#include <nuttx/net/dns.h>
#endif
#endif

#define REQUEST_CAPACITY 512

static int usage(void)
{
    puts("朝夕：问答、记事与待办、定时提醒");
    puts("  zhaoxi [--text] ask <问题>");
    puts("  zhaoxi [--text] note [add] <要记住的内容>");
    puts("  zhaoxi [--text] note list");
    puts("  zhaoxi [--text] todo add <待办>");
    puts("  zhaoxi [--text] todo list");
    puts("  zhaoxi [--text] todo done <编号>");
    puts("  zhaoxi [--text] remind <时间和提醒内容>");
    puts("  zhaoxi [--text] remind list");
    puts("  zhaoxi [--text] remind cancel <任务编号>");
    puts("  zhaoxi status");
    puts("  zhaoxi stop   （请求停止 Agent，等待 Shutdown complete 后再更新资产）");
    puts("  zhaoxi listen start|stop");
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    puts("  zhaoxi test-speaker   （两秒本机提示音，不需要网络）");
#endif
    puts("  zhaoxi say <文本>    （直接测试语音合成与播放）");
    puts("默认通过现有语音通道回复；--text 使用串口文字回复。");
    return 0;
}

static int append_arguments(char *buffer, size_t capacity,
                            int argc, char **argv, int start)
{
    size_t used = strlen(buffer);
    if (start >= argc) return -EINVAL;
    for (int i = start; i < argc; i++) {
        size_t length = strlen(argv[i]);
        if (!length) return -EINVAL;
        for (size_t j = 0; j < length; j++) {
            if ((unsigned char)argv[i][j] < 32) return -EINVAL;
        }
        size_t separator = i > start ? 1 : 0;
        if (length + separator >= capacity - used) return -E2BIG;
        if (separator) buffer[used++] = ' ';
        memcpy(buffer + used, argv[i], length + 1);
        used += length;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char request[REQUEST_CAPACITY] = "";
    const char *prefix = "";
    const char *channel = "voice";
    int at = 1;
    int first_text = 0;
    int needs_text = 1;
    if (argc < 2 || !strcmp(argv[at], "help")) return usage();
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    if (!strcmp(argv[at], "--wifi-mac")) {
        /* This BSP exports the query but does not ship its public header.
         * The devnum, output-buffer and signed return ABI were checked in
         * the AP/RPC implementation. This operation changes no address. */
        extern int bwifi_get_dev_mac(uint8_t devnum, uint8_t *address);
        if (argc != 2) return 1;
        uint8_t radio[6] = {0}, kernel[6] = {0};
        int radio_result = bwifi_get_dev_mac(0, radio);
        int kernel_result = netlib_getmacaddr("wlan0", kernel);
        int equal = !radio_result && !kernel_result && !memcmp(radio, kernel, 6);
        printf("WIFI_MAC radio_rc=%d kernel_rc=%d equal=%d\n", radio_result, kernel_result, equal);
        printf("radio=%02x:%02x:%02x:%02x:%02x:%02x kernel=%02x:%02x:%02x:%02x:%02x:%02x\n",
            radio[0], radio[1], radio[2], radio[3], radio[4], radio[5],
            kernel[0], kernel[1], kernel[2], kernel[3], kernel[4], kernel[5]);
        return radio_result || kernel_result ? 1 : 0;
    }
#ifdef CONFIG_NETDB_DNSCLIENT
    if (!strcmp(argv[at], "--dns")) {
        if (argc != 3) return 1;
        struct sockaddr_in server = { .sin_family = AF_INET };
        if (inet_pton(AF_INET, argv[2], &server.sin_addr) != 1) return 1;
        int result = dns_add_nameserver((const struct sockaddr *)&server, sizeof(server));
        printf("DNS configuration rc=%d; interface address unchanged.\n", result);
        return result ? 1 : 0;
    }
#endif
    if (!strcmp(argv[at], "--set-time")) {
        if (argc != 3) return 1;
        char *end = NULL;
        errno = 0;
        long long epoch = strtoll(argv[2], &end, 10);
        if (errno || !end || *end || epoch < 1735689600LL || epoch > 4102444800LL) return 1;
        struct timespec value = { .tv_sec = epoch, .tv_nsec = 0 };
        int result = clock_settime(CLOCK_REALTIME, &value);
        printf("UTC clock set rc=%d epoch=%lld\n", result, (long long)time(NULL));
        return result ? 1 : 0;
    }
    if (!strcmp(argv[at], "--tool")) {
        /* Board diagnostics exercise the same registered tools as the Agent.
         * Input is a bounded JSON file in tmpfs, never an embedded credential. */
        const char *allowed[] = {"get_current_time", "read_file", "write_file",
                                 "cron_add", "cron_list", "cron_remove"};
        if (argc != 4 || !message_bus_is_ready() || strncmp(argv[3], "/tmp/", 5)) return 1;
        int permitted = 0;
        for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
            if (!strcmp(argv[2], allowed[i])) permitted = 1;
        }
        if (!permitted) return 1;
        char *input = malloc(4096), *output = calloc(1, 8192);
        if (!input || !output) { free(input); free(output); return 1; }
        FILE *file = fopen(argv[3], "r");
        if (!file) { free(input); free(output); return 1; }
        size_t size = fread(input, 1, 4096, file);
        int failed = ferror(file) || size == 4096;
        fclose(file);
        if (failed) { free(input); free(output); return 1; }
        input[size] = 0;
        int result = tool_registry_execute(argv[2], input, output, 8192);
        printf("TOOL_RESULT rc=%d\n%s\n", result, output);
        free(input); free(output);
        return result ? 1 : 0;
    }
    if (!strcmp(argv[at], "test-speaker")) {
        if (argc != 2) return 1;
        static const int16_t wave[16] = {0, 12537, 23165, 30266, 32760, 30266,
            23165, 12537, 0, -12537, -23165, -30266, -32760, -30266, -23165, -12537};
        int16_t *pcm = malloc(3200);
        if (!pcm) return 1;
        for (int i = 0; i < 1600; i++) pcm[i] = wave[i % 16];
        audio_playback_t *pb = audio_playback_open(NULL, 16000, 1, 16);
        int result = pb ? 0 : -1;
        for (int i = 0; pb && !result && i < 20; i++) {
            if (audio_playback_write(pb, pcm, 3200) != 3200) result = -1;
        }
        audio_playback_close(pb);
        free(pcm);
        puts(result ? "本机提示音失败，输出已关闭。" : "两秒本机提示音结束，输出已关闭。");
        return result ? 1 : 0;
    }
#endif
    if (!strcmp(argv[at], "say")) {
        if (!message_bus_is_ready() || append_arguments(request, sizeof(request), argc, argv, at + 1)) return 1;
        agent_msg_t message = {0};
        strcpy(message.channel, "voice");
        strcpy(message.chat_id, "zhaoxi");
        message.content = strdup(request);
        if (!message.content) return 1;
        if (message_bus_push_outbound(&message) != 0) {
            message_bus_msg_free(&message);
            return 1;
        }
        puts("语音合成测试已提交，等待播放结果。");
        return 0;
    }
    if (!strcmp(argv[at], "stop")) {
        if (argc != 2 || !message_bus_is_ready()) return 1;
        agent_request_shutdown();
        puts("已请求停止 Agent；请等 Agent 日志确认实际退出后再更新资产或重新启动。");
        return 0;
    }
    if (!strcmp(argv[at], "listen")) {
        if (argc != 3 || !message_bus_is_ready()) return 1;
        int result;
        if (!strcmp(argv[2], "start")) {
            result = voice_channel_start();
            if (!result) puts("录音已开始，请说话。");
        } else if (!strcmp(argv[2], "stop")) {
            result = voice_channel_stop();
            if (!result) puts("录音已结束，识别和处理结果由助手返回。");
        } else return 1;
        if (result) puts("语音操作失败，请检查服务状态。");
        return result ? 1 : 0;
    }
    if (!strcmp(argv[at], "status")) {
        if (argc != 2) return 1;
        puts(message_bus_is_ready() ? "Agent 消息总线已启动。" : "Agent 尚未启动或正在停止。");
        puts("网络、模型和语音服务状态以实际请求结果为准。");
        return message_bus_is_ready() ? 0 : 1;
    }
    if (!strcmp(argv[at], "--text")) {
        channel = "cli";
        if (++at >= argc) return 1;
    }
    if (!strcmp(argv[at], "ask")) {
        first_text = at + 1;
    } else if (!strcmp(argv[at], "note")) {
        prefix = "请使用智能记事本记录这条备忘，并在工具写入成功后确认：";
        first_text = at + 1;
        if (first_text < argc && !strcmp(argv[first_text], "list") && first_text + 1 == argc) {
            prefix = "请使用智能记事本读取今天的真实备忘，简短回答。";
            needs_text = 0;
        } else if (first_text < argc && !strcmp(argv[first_text], "add")) {
            first_text++;
        }
    } else if (!strcmp(argv[at], "remind")) {
        prefix = "请使用智能提醒设置提醒，先核对当前时间，只在创建成功后确认：";
        first_text = at + 1;
        if (first_text < argc && !strcmp(argv[first_text], "list") && first_text + 1 == argc) {
            prefix = "请使用智能提醒，通过 cron_list 读取当前提醒并简短回答。";
            needs_text = 0;
        } else if (first_text < argc && !strcmp(argv[first_text], "cancel")) {
            if (first_text + 2 != argc) return 1;
            const char *id = argv[first_text + 1];
            if (!id[0] || strlen(id) > 8 || strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(id)) return 1;
            prefix = "请使用智能提醒核对并取消这个任务编号，只在 cron_remove 成功后确认：";
            first_text++;
        }
    } else if (!strcmp(argv[at], "todo")) {
        if (++at >= argc) return 1;
        if (!strcmp(argv[at], "add")) {
            prefix = "请使用智能记事本添加待办，保留其他记录：";
            first_text = at + 1;
        } else if (!strcmp(argv[at], "list") && at + 1 == argc) {
            prefix = "请使用智能记事本读取并列出当前未完成待办，按编号简短回答。";
            first_text = argc;
            needs_text = 0;
        } else if (!strcmp(argv[at], "done") && at + 2 == argc) {
            char *end = NULL;
            errno = 0;
            unsigned long number = strtoul(argv[at + 1], &end, 10);
            if (errno || !number || !end || *end || number > 9999 || argv[at + 1][0] == '-') return 1;
            prefix = "请使用智能记事本读取待办，只完成指定编号并保留其他记录。编号：";
            first_text = at + 1;
        } else {
            return 1;
        }
    } else {
        usage();
        return 1;
    }

    if (strlen(prefix) >= sizeof(request)) return 1;
    strcpy(request, prefix);
    if (needs_text) {
        int result = append_arguments(request, sizeof(request), argc, argv, first_text);
        if (result) {
            puts(result == -E2BIG ? "内容过长，请缩短后重试。" : "请提供完整内容。");
            return 1;
        }
    }
    if (!request[0] || !message_bus_is_ready()) {
        puts("请先启动 ai_agent --headless，再提交请求。");
        return 1;
    }
    agent_msg_t message = {0};
    strncpy(message.channel, channel, sizeof(message.channel) - 1);
    strncpy(message.chat_id, "zhaoxi", sizeof(message.chat_id) - 1);
    message.content = strdup(request);
    if (!message.content) return 1;
    if (message_bus_push_inbound(&message) != 0) {
        message_bus_msg_free(&message);
        puts("请求未提交，请稍后重试。");
        return 1;
    }
    puts("请求已提交，等待助手确认。");
    return 0;
}

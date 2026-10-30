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
    if (!strcmp(argv[at], "stop")) {
        if (argc != 2 || !message_bus_is_ready()) return 1;
        agent_request_shutdown();
        puts("已请求停止 Agent；请等待 Shutdown complete 后再更新资产或重新启动。");
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

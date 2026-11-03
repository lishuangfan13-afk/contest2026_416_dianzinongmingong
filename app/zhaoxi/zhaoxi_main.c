/* SPDX-License-Identifier: Apache-2.0
 * Screen-free product entry. The AI Agent owns tools, storage and speech.
 */

#include "core/message_bus.h"
#include "voice/voice_channel.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "todo_store.h"
#include "journal.h"
#include "local_intent.h"
#ifdef CONFIG_ZHAOXI_MIMO_VOICE
#include "local_prompts.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef CONFIG_NETUTILS_PPPD
#include "pc_link.h"
#endif
#ifdef CONFIG_ZHAOXI_BES_AUDIO
#include "reminder.h"
#include "drink.h"
#include "morning.h"
#include "notifications/feishu_notify.h"
#include "notifications/notify_service.h"
#include "wake_audio_diag.h"
#include "infra/config_store.h"
#include "dns_diag.h"
#include "voice/audio_playback.h"
#include "voice/voice_asr.h"
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
    puts("  zhaoxi [--text] remind in <秒数> <内容> （本机直接创建）");
    puts("  zhaoxi [--text] remind list");
    puts("  zhaoxi [--text] remind cancel <任务编号>");
    puts("  zhaoxi status");
    puts("  zhaoxi stop   （请求停止 Agent，等待 Shutdown complete 后再更新资产）");
    puts("  zhaoxi listen start|stop");
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    puts("  zhaoxi profile show | set <长期人设>");
    puts("  zhaoxi news status | fetch | ask | play | endpoint <URL>");
    puts("  zhaoxi news play today|yesterday （重播今天或昨天保存的新闻）");
    puts("  zhaoxi auto start|stop|status （启动一次，自动收音与回答）");
    puts("  zhaoxi auto wake on|off|status （ASR唤醒短语开关）");
    puts("  zhaoxi [--text] drink start [分钟] | stop | status （默认60分钟）");
    puts("  zhaoxi notify status | test <事件编号> | send <事件编号> <内容>");
    puts("  zhaoxi notify news today|yesterday | todos （加入飞书后台发送队列）");
#endif
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    puts("  zhaoxi test-speaker   （两秒本机提示音，不需要网络）");
#endif
    puts("  zhaoxi say <文本>    （直接测试语音合成与播放）");
#ifdef CONFIG_ZHAOXI_MIMO_VOICE
    puts("  zhaoxi prompt status | play hello|news_ready|network_error|network_connected （本地提示音）");
#endif
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

static int todo_command(int argc, char **argv, int at, const char *channel)
{
    if (at + 1 >= argc) return 1;
    const char *operation = argv[at + 1];
    char content[REQUEST_CAPACITY] = "";
    const char *argument = NULL;
    if (!strcmp(operation, "add")) {
        if (append_arguments(content, sizeof(content), argc, argv, at + 2)) return 1;
        argument = content;
    } else if (!strcmp(operation, "done") && at + 3 == argc) argument = argv[at + 2];
    else if (strcmp(operation, "list") || at + 2 != argc) return 1;
    if (!strcmp(channel, "voice") && !message_bus_is_ready()) {
        puts("Agent 尚未启动，未执行语音待办操作；可用 --text 本地管理。");
        return 1;
    }
    struct zhaoxi_todo_result result;
    char *reply = NULL;
    const char *path = AGENT_DATA_DIR "/TASKS.md";
    int rc = zhaoxi_todo_execute(path, operation, argument, &result, &reply);
    printf("TODO_RESULT op=%s rc=%d", operation, rc);
    if (!rc && !strcmp(operation, "add")) printf(" id=%u", result.id);
    if (!rc && !strcmp(operation, "done")) printf(" id=%u changed=%d", result.id, result.changed);
    if (!rc && !strcmp(operation, "list")) printf(" count=%u", result.count);
    printf(" path=%s\n", path);
    if (rc) {
        const char *error = !strcmp(operation, "list") ? "待办读取失败，未返回猜测内容。" :
                            "待办操作未保存：请检查编号、文件格式、容量与存储状态。";
        puts(error);
        if (!strcmp(channel, "voice")) {
            agent_msg_t message = {0};
            strcpy(message.channel, "voice");
            strcpy(message.chat_id, "zhaoxi");
            message.content = strdup(error);
            if (message.content && message_bus_push_outbound(&message)) message_bus_msg_free(&message);
        }
        return 1;
    }
    puts(reply);
    if (!strcmp(channel, "voice")) {
        agent_msg_t message = {0};
        strcpy(message.channel, "voice");
        strcpy(message.chat_id, "zhaoxi");
        message.content = reply;
        if (message_bus_push_outbound(&message)) {
            message_bus_msg_free(&message);
            puts("待办操作已完成，但语音回复未提交；请用 --text 查询结果。");
            return 1;
        }
    } else free(reply);
    return 0;
}

static int note_command(int argc, char **argv, int at, const char *channel)
{
    int start = at + 1;
    const char *operation = "add";
    if (start < argc && !strcmp(argv[start], "list") && start + 1 == argc)
        operation = "list";
    else if (start < argc && !strcmp(argv[start], "add")) start++;
    char content[REQUEST_CAPACITY] = "";
    if (!strcmp(operation, "add") && append_arguments(content, sizeof(content), argc, argv, start)) return 1;
    int voice = !strcmp(channel, "voice");
    if (voice && !message_bus_is_ready()) {
        puts("Agent 尚未启动，未执行语音记事操作；可用 --text 本地管理。");
        return 1;
    }
    time_t now = time(NULL);
    char path[256] = "";
    zhaoxi_note_path(now, path, sizeof(path));
    struct zhaoxi_note_result result;
    char *reply = NULL;
    int rc = zhaoxi_note_execute(now, operation, content, &result, &reply);
    printf("NOTE_RESULT op=%s rc=%d verified=%d uncertain=%d bytes=%zu path=%s\n",
           operation, rc, result.verified, result.storage_uncertain, result.bytes, path);
    const char *error = !strcmp(operation, "list") ? "备忘读取失败，请检查时钟和存储状态。" :
        "未确认保存备忘；请先查询今日备忘，避免重复添加，并检查时钟、容量和存储状态。";
    if (rc) {
        puts(error);
        if (voice) reply = strdup(error);
        else return 1;
    } else puts(reply);
    if (voice && reply) {
        agent_msg_t message = {0};
        strcpy(message.channel, "voice"); strcpy(message.chat_id, "zhaoxi");
        message.content = reply;
        if (message_bus_push_outbound(&message)) {
            message_bus_msg_free(&message);
            puts("记事结果已在串口输出，但语音回复未提交；请用 --text 查询。");
            return 1;
        }
    } else free(reply);
    return rc ? 1 : 0;
}

#ifdef CONFIG_ZHAOXI_BES_AUDIO
static int notify_command(int argc, char **argv, int at)
{
    if (at + 1 >= argc) return 1;
    const char *op = argv[at + 1];
    if (!strcmp(op, "status") && argc == at + 2) {
        char value[512];
        const char *keys[] = {"feishu_notify_enabled", "feishu_app_id",
                              "feishu_app_secret", "feishu_notify_receive_id"};
        int present[4] = {0};
        for (int i = 0; i < 4; i++) {
            memset(value, 0, sizeof(value));
            present[i] = claw_config_get(keys[i], value, sizeof(value)) == 0 && value[0];
            if (i == 0) present[i] = present[i] && (!strcmp(value, "1") || !strcmp(value, "true"));
        }
        printf("NOTIFY_CONFIG enabled=%d app_id_set=%d app_secret_set=%d target_set=%d\n",
               present[0], present[1], present[2], present[3]);
        memset(value, 0, sizeof(value));
        char state[320];if(!zhaoxi_notify_status(state,sizeof(state)))puts(state);
        return 0;
    }
    if((!strcmp(op,"news") && argc==at+3 &&
        (!strcmp(argv[at+2],"today") || !strcmp(argv[at+2],"yesterday"))) ||
       (!strcmp(op,"todos") && argc==at+2)) {
        int result=!strcmp(op,"todos")?zhaoxi_notify_todos():zhaoxi_notify_news(!strcmp(argv[at+2],"yesterday"));
        printf("NOTIFY_ENQUEUE rc=%d\n",result);
        puts(result==-EALREADY?"同批内容已经入队或发送过，请在飞书查看。":
            result?"未加入飞书队列，请检查配置、缓存与队列状态。":"已加入飞书队列；发送结果请查询 notify status。");
        return result && result!=-EALREADY?1:0;
    }
    struct zhaoxi_feishu_receipt receipt = {0};
    int rc;
    if (!strcmp(op, "test") && argc == at + 3)
        rc = zhaoxi_feishu_notify_test(argv[at + 2], &receipt);
    else if (!strcmp(op, "send") && argc > at + 3) {
        char text[REQUEST_CAPACITY] = "";
        if (append_arguments(text, sizeof(text), argc, argv, at + 3)) return 1;
        rc = zhaoxi_feishu_notify(text, argv[at + 2], &receipt);
    } else return 1;
    printf("NOTIFY_RESULT rc=%d phase=%d http=%d api=%d attempted=%d accepted=%d unknown=%d\n",
           rc, receipt.phase, receipt.http_status, receipt.api_code,
           receipt.send_attempted, receipt.accepted, receipt.delivery_unknown);
    if (!rc && receipt.accepted) puts("飞书通知已发送。");
    else if (receipt.delivery_unknown) puts("通知状态待确认；检查飞书消息后再决定是否重试。");
    else puts("飞书通知未发送，请检查通知开关、凭据和固定目标配置。");
    return rc || !receipt.accepted ? 1 : 0;
}
#endif

int main(int argc, char **argv)
{
    zhaoxi_local_intent_link();
    char request[REQUEST_CAPACITY] = "";
    const char *prefix = "";
    const char *channel = "voice";
    int at = 1;
    int first_text = 0;
    int needs_text = 1;
    if (argc < 2 || !strcmp(argv[at], "help")) return usage();
#ifdef CONFIG_ZHAOXI_MIMO_VOICE
    if(!strcmp(argv[at],"prompt")) {
        if(argc==3 && !strcmp(argv[2],"status")) {
            char status[256];int rc=zhaoxi_prompt_status(status,sizeof(status));
            if(!rc)fputs(status,stdout);return rc?1:0;
        }
        if(argc==4 && !strcmp(argv[2],"play") && message_bus_is_ready()) {
            const char *text=zhaoxi_prompt_text(argv[3]);size_t bytes;
            int rc=text?zhaoxi_prompt_check(argv[3],&bytes):-EINVAL;
            if(!rc)rc=voice_channel_speak(text);
            printf("PROMPT_RESULT rc=%d\n",rc);return rc?1:0;
        }
        if(argc==5 && !strcmp(argv[2],"receive") && !message_bus_is_ready()) {
            char *end;unsigned long length=strtoul(argv[4],&end,10);
            if(!argv[4][0] || *end || length>480024)return 1;
            return zhaoxi_prompt_receive(argv[3],length)?1:0;
        }
        return usage();
    }
#endif
#ifdef CONFIG_NETUTILS_PPPD
    if (!strcmp(argv[at], "pc-link")) {
        if (argc != 3) return 1;
        if (!strcmp(argv[2], "serve")) return zhaoxi_pc_link(0) ? 1 : 0;
        char *end = NULL;
        unsigned long seconds = strtoul(argv[2], &end, 10);
        if (!end || *end || seconds < 45 || seconds > 600) return 1;
        return zhaoxi_pc_link((unsigned)seconds) ? 1 : 0;
    }
#endif
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    if(!strcmp(argv[at],"--wake-audio"))
        return argc==at+2 && !zhaoxi_wake_audio_command(argv[at+1])?0:1;
    if (!strcmp(argv[at], "--asr-probe")) {
        if(argc!=3 || !message_bus_is_ready())return 1;
        char *end=NULL;long milliseconds=strtol(argv[2],&end,10);
        if(!end || *end || milliseconds<1000 || milliseconds>6000)return 1;
        char status[1400];if(voice_channel_auto_status(status,sizeof(status)))return 1;
        if(strstr(status,"\"enabled\":true") || strstr(status,"\"running\":true") ||
           strstr(status,"\"output_active\":true")) {
            puts("ASR_PROBE refused: stop automatic voice and wait for output to finish.");return 1;
        }
        size_t size=(size_t)milliseconds*32;unsigned char *silence=calloc(1,size);
        if(!silence)return 1;
        char text[1024];int rc=voice_asr_recognize(silence,size,text,sizeof(text));free(silence);
        printf("ASR_PROBE synthetic_silence_ms=%ld bytes=%zu rc=%d text_bytes=%zu\n",
               milliseconds,size,rc,rc?0:strlen(text));return rc?1:0;
    }
    if (!strcmp(argv[at], "news")) return zhaoxi_morning_command(argc-at+1,argv+at-1);
    if (!strcmp(argv[at], "profile")) {
        char profile[MORNING_PROFILE_MAX+1]={0};int rc=-EINVAL;
        if(argc==at+2 && !strcmp(argv[at+1],"show")) {
            rc=morning_profile_get(profile,sizeof(profile));if(!rc)puts(profile);
        } else if(argc>at+2 && !strcmp(argv[at+1],"set")) {
            rc=append_arguments(profile,sizeof(profile),argc,argv,at+2);
            if(!rc)rc=zhaoxi_morning_set_profile(profile);
        }
        printf("PROFILE_RESULT rc=%d\n",rc);return rc?1:0;
    }
    if (!strcmp(argv[at], "auto")) {
        if (argc == 4 && !strcmp(argv[2], "debug")) {
            if (strcmp(argv[3], "on") && strcmp(argv[3], "off")) return 1;
            return voice_channel_auto_wake_debug_set_enabled(!strcmp(argv[3], "on")) ? 1 : 0;
        }
        if (argc == 4 && !strcmp(argv[2], "wake")) {
            if (!strcmp(argv[3], "status")) {
                bool enabled = false, awake = false;
                int rc = voice_channel_auto_wake_get_state(&enabled, &awake);
                printf("WAKE_RESULT rc=%d enabled=%d awake=%d\n", rc, enabled, awake);
                return rc ? 1 : 0;
            }
            if (strcmp(argv[3], "on") && strcmp(argv[3], "off")) return 1;
            int rc = voice_channel_auto_wake_set_enabled(!strcmp(argv[3], "on"));
            printf("WAKE_RESULT rc=%d enabled=%d\n", rc, !strcmp(argv[3], "on"));
            return rc ? 1 : 0;
        }
        if (argc != 3) return 1;
        if (!strcmp(argv[2], "start")) {
            if (!message_bus_is_ready()) return 1;
            int result = voice_channel_auto_start();
            puts(result ? "自动语音启动失败，请查看服务状态。" : "自动语音已启动，校准环境后等待你说话。");
            return result ? 1 : 0;
        }
        if (!strcmp(argv[2], "stop")) return voice_channel_auto_stop() ? 1 : 0;
        if (!strcmp(argv[2], "status")) {
            char status[1024]; int result = voice_channel_auto_status(status, sizeof(status));
            puts(status); return result ? 1 : 0;
        }
        return 1;
    }
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
    if (!strcmp(argv[at], "--dns-info")) {
        return argc == 2 && !zhaoxi_dns_info() ? 0 : 1;
    }
    if (!strcmp(argv[at], "--dns-selftest")) {
        return argc == 2 && !zhaoxi_dns_selftest() ? 0 : 1;
    }
    if (!strcmp(argv[at], "--dns-probe")) {
        return argc == 4 && !zhaoxi_dns_probe(argv[2], argv[3]) ? 0 : 1;
    }
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
        int result = zhaoxi_play_alert();
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
    if (!strcmp(argv[at], "todo")) return todo_command(argc, argv, at, channel);
    if (!strcmp(argv[at], "note")) return note_command(argc, argv, at, channel);
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    if (!strcmp(argv[at], "notify")) return notify_command(argc, argv, at);
    if (!strcmp(argv[at], "drink"))
        return zhaoxi_drink_command(argc, argv, at, channel);
    if (!strcmp(argv[at], "remind") && at + 1 < argc &&
        (!strcmp(argv[at + 1], "in") || !strcmp(argv[at + 1], "list") ||
         !strcmp(argv[at + 1], "cancel"))) {
        return zhaoxi_reminder_command(argc, argv, at, channel);
    }
#endif
    if (!strcmp(argv[at], "ask")) {
        first_text = at + 1;
    } else if (!strcmp(argv[at], "remind")) {
#ifdef CONFIG_ZHAOXI_BES_AUDIO
        char original[REQUEST_CAPACITY] = "";
        struct zhaoxi_local_intent local;
        if (!append_arguments(original, sizeof(original), argc, argv, at + 1) &&
            zhaoxi_local_parse(original, &local) && local.kind == ZHAOXI_LOCAL_REMIND_IN) {
            /* Keep the exact natural request for the Agent's local hook. */
            first_text = at + 1;
        } else
#endif
        {
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

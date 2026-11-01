/* SPDX-License-Identifier: Apache-2.0 */
#include "core/message_bus.h"
#include "agent_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int zhaoxi_main(int argc, char **argv);
/* Explicit hardware test doubles: these do not verify real audio. */
static int audio_result = -1;
static int started, stopped;
static int shutdown_requests;
void agent_request_shutdown(void) { shutdown_requests++; }
int voice_channel_start(void) { started++; return audio_result; }
int voice_channel_stop(void) { stopped++; return audio_result; }

static void no_message(void)
{
    agent_msg_t message = {0};
    assert(message_bus_pop_inbound(&message, 0) != 0);
}

static void expect(int argc, char **argv, const char *channel, const char *text)
{
    agent_msg_t message = {0};
    assert(zhaoxi_main(argc, argv) == 0);
    assert(message_bus_pop_inbound(&message, 0) == 0);
    assert(strcmp(message.channel, channel) == 0);
    assert(strcmp(message.chat_id, "zhaoxi") == 0);
    assert(strstr(message.content, text) != NULL);
    assert(message.image_b64 == NULL);
    message_bus_msg_free(&message);
    no_message();
}

static void expect_local_voice(int argc, char **argv, const char *text)
{
    assert(zhaoxi_main(argc, argv) == 0);
    no_message(); /* Explicit TODO operations must never call the model. */
    agent_msg_t reply = {0};
    assert(message_bus_pop_outbound(&reply, 0) == 0);
    assert(!strcmp(reply.channel, "voice") && strstr(reply.content, text));
    message_bus_msg_free(&reply);
}

int main(void)
{
    mkdir(AGENT_DATA_DIR, 0700);
    unlink(AGENT_DATA_DIR "/TASKS.md"); /* This executable's isolated test fixture. */
    char *ask[] = {"zhaoxi", "ask", "hello", "world"};
    assert(!message_bus_is_ready());
    char *shutdown[] = {"zhaoxi", "stop"};
    assert(zhaoxi_main(2, shutdown) != 0);
    assert(shutdown_requests == 0);
    assert(zhaoxi_main(4, ask) != 0);
    message_bus_wakeup(); /* Also safe before initialization. */
    assert(message_bus_init() == 0);
    assert(message_bus_is_ready());
    char *record[] = {"zhaoxi", "listen", "start"};
    char *stop[] = {"zhaoxi", "listen", "stop"};
    assert(zhaoxi_main(3, record) != 0);
    audio_result = 0;
    assert(zhaoxi_main(3, record) == 0);
    assert(zhaoxi_main(3, stop) == 0);
    assert(started == 2 && stopped == 1);
    no_message();
    char *say[] = {"zhaoxi", "say", "speaker test"};
    agent_msg_t spoken = {0};
    assert(zhaoxi_main(3, say) == 0);
    assert(message_bus_pop_outbound(&spoken, 0) == 0);
    assert(!strcmp(spoken.channel, "voice") && !strcmp(spoken.content, "speaker test"));
    message_bus_msg_free(&spoken);
    no_message();
    expect(4, ask, "voice", "hello world");
    char *text[] = {"zhaoxi", "--text", "ask", "你好"};
    expect(4, text, "cli", "你好");
    char *note[] = {"zhaoxi", "note", "图书馆借书"};
    expect(3, note, "voice", "图书馆借书");
    char *notes[] = {"zhaoxi", "note", "list"};
    expect(3, notes, "voice", "读取今天的真实备忘");
    char *add[] = {"zhaoxi", "todo", "add", "交作业"};
    expect_local_voice(4, add, "已添加待办 [1]：交作业");
    char *list[] = {"zhaoxi", "todo", "list"};
    expect_local_voice(3, list, "[1] 交作业");
    char *done[] = {"zhaoxi", "todo", "done", "1"};
    expect_local_voice(4, done, "待办 [1] 已完成并保存");
    char *local_text[] = {"zhaoxi", "--text", "todo", "add", "带水杯"};
    assert(zhaoxi_main(5, local_text) == 0);no_message();
    agent_msg_t no_speech = {0};
    assert(message_bus_pop_outbound(&no_speech, 0) != 0);
    expect_local_voice(3, list, "[2] 带水杯");
    char *remind[] = {"zhaoxi", "remind", "十分钟后提醒我交作业"};
    expect(3, remind, "voice", "十分钟后提醒我交作业");
    char *reminders[] = {"zhaoxi", "remind", "list"};
    expect(3, reminders, "voice", "cron_list");
    char *cancel[] = {"zhaoxi", "remind", "cancel", "a123b4"};
    expect(4, cancel, "voice", "取消这个任务编号");

    char *empty[] = {"zhaoxi", "note"};
    char *bad[] = {"zhaoxi", "todo", "done", "-1"};
    char *zero[] = {"zhaoxi", "todo", "done", "0"};
    char *unknown[] = {"zhaoxi", "unknown", "text"};
    char *extra[] = {"zhaoxi", "todo", "list", "extra"};
    char *control[] = {"zhaoxi", "ask", "hello\nworld"};
    char *bad_id[] = {"zhaoxi", "remind", "cancel", "../jobs"};
    assert(zhaoxi_main(2, empty) != 0);
    assert(zhaoxi_main(4, bad) != 0);
    assert(zhaoxi_main(4, zero) != 0);
    assert(zhaoxi_main(3, unknown) != 0);
    assert(zhaoxi_main(4, extra) != 0);
    assert(zhaoxi_main(3, control) != 0);
    assert(zhaoxi_main(4, bad_id) != 0);
    char huge[1024];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    char *oversize[] = {"zhaoxi", "ask", huge};
    assert(zhaoxi_main(3, oversize) != 0);
    no_message();

    assert(zhaoxi_main(2, shutdown) == 0);
    assert(shutdown_requests == 1);
    no_message();
    for (int i = 0; i < 16; i++) assert(zhaoxi_main(4, ask) == 0);
    assert(zhaoxi_main(4, ask) != 0); /* Full bus: caller must free rejected request. */
    for (int i = 0; i < 16; i++) {
        agent_msg_t message = {0};
        assert(message_bus_pop_inbound(&message, 0) == 0);
        message_bus_msg_free(&message);
    }
    message_bus_wakeup();
    assert(!message_bus_is_ready());
    assert(zhaoxi_main(4, ask) != 0);
    message_bus_destroy();
    unlink(AGENT_DATA_DIR "/TASKS.md");
    assert(!message_bus_is_ready());
    puts("PASS: routes, UTF-8, validation, full queue, pre-init, shutdown and audio API error propagation.");
    return 0;
}

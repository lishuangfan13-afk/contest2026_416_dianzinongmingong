/* SPDX-License-Identifier: Apache-2.0 */
#include "local_intent.h"
#include "journal.h"
#include "todo_store.h"
#include "agent_config.h"
#ifdef CONFIG_ZHAOXI_BES_AUDIO
#include "reminder.h"
#include "drink.h"
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void zhaoxi_local_intent_link(void) { }

static const char *skip_space(const char *text)
{
    while (*text) {
        if (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
        else if (!strncmp(text, "　", strlen("　"))) text += strlen("　");
        else break;
    }
    return text;
}
static const char *skip_separator(const char *text)
{
    for (;;) {
        text = skip_space(text);
        if (*text == ':' || *text == ',') text++;
        else if (!strncmp(text, "：", strlen("："))) text += strlen("：");
        else if (!strncmp(text, "，", strlen("，"))) text += strlen("，");
        else break;
    }
    return text;
}
static size_t trimmed_length(const char *text)
{
    size_t size = strlen(text);
    for (;;) {
        if (size && (text[size-1] == ' ' || text[size-1] == '\t' ||
                     text[size-1] == '\r' || text[size-1] == '\n' ||
                     text[size-1] == '.' || text[size-1] == '!')) { size--; continue; }
        const char *ends[] = {"。", "！", "　", NULL};
        int changed = 0;
        for (int i = 0; ends[i]; i++) {
            size_t n = strlen(ends[i]);
            if (size >= n && !memcmp(text + size - n, ends[i], n)) {
                size -= n; changed = 1; break;
            }
        }
        if (!changed) return size;
    }
}
static int is_question(const char *text)
{
    const char *words[] = {"?", "？", "怎么", "如何", "为什么", "是否", "能不能",
        "可不可以", "有没有", "能否", "可否", "什么", "哪个", "哪些", "多少", "几点", "几号", NULL};
    for (int i = 0; words[i]; i++) if (strstr(text, words[i])) return 1;
    size_t size = trimmed_length(text);
    const char *ends[] = {"吗", "呢", "么", NULL};
    for (int i = 0; ends[i]; i++) {
        size_t n = strlen(ends[i]);
        if (size >= n && !memcmp(text + size - n, ends[i], n)) return 1;
    }
    return 0;
}
static int copy_content(struct zhaoxi_local_intent *intent,
                        enum zhaoxi_local_kind kind, const char *body)
{
    body = skip_separator(body);
    /* Only command separators and outer whitespace are grammar. Preserve
     * punctuation in the actual note/task body, and never truncate it. */
    size_t size = strlen(body);
    for (;;) {
        if (size && (body[size-1] == ' ' || body[size-1] == '\t' ||
                     body[size-1] == '\r' || body[size-1] == '\n')) { size--; continue; }
        size_t wide = strlen("　");
        if (size >= wide && !memcmp(body + size - wide, "　", wide)) { size -= wide; continue; }
        break;
    }
    if (!size || size >= sizeof(intent->content)) return 0;
    /* Explanations about a command are not requests to execute it. */
    if (!strncmp(body, "的", strlen("的"))) return 0;
    for (size_t i = 0; i < size; i++)
        if ((unsigned char)body[i] < 32 || (unsigned char)body[i] == 127) return 0;
    intent->kind = kind;
    memcpy(intent->content, body, size); intent->content[size] = '\0';
    return 1;
}
static int parse_id(const char *text, const char **tail, unsigned *id)
{
    text = skip_space(text);
    if (!strncmp(text, "编号", strlen("编号"))) text = skip_space(text + strlen("编号"));
    unsigned value = 0; const char *start = text;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (unsigned)(*text++ - '0');
        if (value > 9999) return 0;
    }
    if (text == start || !value) return 0;
    *id = value; *tail = skip_space(text); return 1;
}

static unsigned chinese_digit(const char **text)
{
    const char *words[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "两", NULL};
    for (unsigned i = 0; words[i]; i++)
        if (!strncmp(*text, words[i], strlen(words[i]))) {
            *text += strlen(words[i]); return i == 9 ? 2 : i + 1;
        }
    return 0;
}
static int parse_relative(const char *text, struct zhaoxi_local_intent *intent)
{
    const char *p = text;
    const char *prefixes[] = {"请帮我", "帮我", "请", NULL};
    for (int i = 0; prefixes[i]; i++)
        if (!strncmp(p, prefixes[i], strlen(prefixes[i]))) { p += strlen(prefixes[i]); break; }
    p = skip_space(p);
    if (!strncmp(p, "在", strlen("在"))) p = skip_space(p + strlen("在"));
    uint32_t number = 0;
    if (*p >= '0' && *p <= '9') {
        do {
            unsigned digit = (unsigned)(*p++ - '0');
            if (number > (31536000U - digit) / 10) return 0;
            number = number * 10 + digit;
        } while (*p >= '0' && *p <= '9');
    } else {
        if (!strncmp(p, "十", strlen("十"))) {
            p += strlen("十"); number = 10 + chinese_digit(&p);
        } else {
            int two_spelling = !strncmp(p, "两", strlen("两"));
            number = chinese_digit(&p);
            if (number && !strncmp(p, "十", strlen("十"))) {
                if (two_spelling) return 0;
                p += strlen("十"); number = number * 10 + chinese_digit(&p);
            }
        }
    }
    if (!number) return 0;
    p = skip_space(p);
    struct {const char *word; unsigned seconds;} units[] = {{"秒钟",1},{"分钟",60},{"小时",3600},{"秒",1}};
    unsigned unit = 0;
    for (unsigned i = 0; i < sizeof(units)/sizeof(units[0]); i++)
        if (!strncmp(p, units[i].word, strlen(units[i].word))) {
            p += strlen(units[i].word); unit = units[i].seconds; break;
        }
    if (!unit || number > 31536000U / unit) return 0;
    p = skip_space(p);
    if (strncmp(p, "后提醒我", strlen("后提醒我"))) return 0;
    p += strlen("后提醒我");
    const char *recurring[] = {"每隔", "每分钟", "每小时", "每天", "每周", "重复", "循环", "定期", NULL};
    for (int i = 0; recurring[i]; i++) if (strstr(p, recurring[i])) return 0;
    if (!copy_content(intent, ZHAOXI_LOCAL_REMIND_IN, p)) return 0;
    if (strlen(intent->content) > 255) { memset(intent, 0, sizeof(*intent)); return 0; }
    intent->delay_seconds = number * unit;
    return 1;
}
static int parse_drink(const char *text, struct zhaoxi_local_intent *intent)
{
    const char *prefixes[] = {"请帮我", "帮我", "请", NULL};
    for (int i = 0; prefixes[i]; i++)
        if (!strncmp(text, prefixes[i], strlen(prefixes[i]))) {
            text = skip_space(text + strlen(prefixes[i])); break;
        }
    struct {const char *text; enum zhaoxi_local_kind kind;} commands[] = {
        {"开启喝水提醒", ZHAOXI_LOCAL_DRINK_START},
        {"开始喝水提醒", ZHAOXI_LOCAL_DRINK_START},
        {"关闭喝水提醒", ZHAOXI_LOCAL_DRINK_STOP},
        {"查询喝水提醒", ZHAOXI_LOCAL_DRINK_STATUS},
        {"喝水提醒状态", ZHAOXI_LOCAL_DRINK_STATUS}
    };
    size_t size = trimmed_length(text);
    for (unsigned i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        if (strlen(commands[i].text) == size && !memcmp(text, commands[i].text, size)) {
            intent->kind = commands[i].kind; return 1;
        }
    }
    return 0;
}
int zhaoxi_local_parse(const char *text, struct zhaoxi_local_intent *intent)
{
    if (!intent) return 0;
    memset(intent, 0, sizeof(*intent));
    if (!text || is_question(text)) return 0;
    text = skip_space(text);
    if (parse_drink(text, intent) || parse_relative(text, intent)) return 1;
    const char *notes[] = {"请帮我记录一下", "请帮我记一下", "帮我记录一下", "帮我记一下",
                           "请记录一下", "记录一下", "记一下", NULL};
    const char *adds[] = {"请帮我添加一个待办", "帮我添加一个待办", "帮我添加一条待办",
                          "添加一个待办", "添加一条待办",
                          "请帮我添加待办", "帮我添加待办", "请添加待办", "添加待办",
                          "帮我新增待办", "新增待办", "帮我加个待办", "加个待办", NULL};
    for (int i = 0; notes[i]; i++)
        if (!strncmp(text, notes[i], strlen(notes[i])))
            return copy_content(intent, ZHAOXI_LOCAL_NOTE_ADD, text + strlen(notes[i]));
    for (int i = 0; adds[i]; i++)
        if (!strncmp(text, adds[i], strlen(adds[i])))
            return copy_content(intent, ZHAOXI_LOCAL_TODO_ADD, text + strlen(adds[i]));
    const char *lists[] = {"帮我查看待办", "查看待办", "查询待办", "列出待办", "待办列表", NULL};
    size_t size = trimmed_length(text);
    for (int i = 0; lists[i]; i++)
        if (strlen(lists[i]) == size && !memcmp(text, lists[i], size)) {
            intent->kind = ZHAOXI_LOCAL_TODO_LIST; return 1;
        }
    const char *done[] = {"帮我完成待办", "完成待办", "把待办", "把编号", NULL};
    for (int i = 0; done[i]; i++) {
        size_t n = strlen(done[i]);
        if (strncmp(text, done[i], n)) continue;
        const char *tail; unsigned id;
        if (!parse_id(text + n, &tail, &id)) return 0;
        size_t remaining = trimmed_length(tail);
        const char *suffixes[] = {"标记为完成", "标为完成", "标完成", "完成", NULL};
        int valid = i < 2 && !remaining;
        for (int j = 0; suffixes[j]; j++)
            if (strlen(suffixes[j]) == remaining && !memcmp(tail, suffixes[j], remaining)) valid = 1;
        if (!valid) return 0;
        intent->kind = ZHAOXI_LOCAL_TODO_DONE;
        snprintf(intent->content, sizeof(intent->content), "%u", id);
        return 1;
    }
    return 0;
}
static const char *match(const agent_msg_t *message, struct zhaoxi_local_intent *intent)
{
    if (!message || !message->content || message->image_b64 ||
        (strcmp(message->channel, "voice") && strcmp(message->channel, "cli")) ||
        !zhaoxi_local_parse(message->content, intent)) return NULL;
    const char *names[] = {NULL, "local_note_add", "local_todo_add", "local_todo_list", "local_todo_done", "local_reminder_add",
                           "local_drink_start", "local_drink_stop", "local_drink_status"};
#ifndef CONFIG_ZHAOXI_BES_AUDIO
    if (intent->kind == ZHAOXI_LOCAL_REMIND_IN || intent->kind >= ZHAOXI_LOCAL_DRINK_START) return NULL;
#endif
    return names[intent->kind];
}
const char *agent_product_local_match(const agent_msg_t *message)
{
    struct zhaoxi_local_intent intent;
    return match(message, &intent);
}
int agent_product_local_execute(const agent_msg_t *message, char **reply)
{
    if (!reply) return -EINVAL;
    *reply = NULL;
    struct zhaoxi_local_intent intent;
    const char *name = match(message, &intent);
    if (!name) return -EINVAL;
    int rc;
    if (intent.kind == ZHAOXI_LOCAL_NOTE_ADD) {
        struct zhaoxi_note_result result;
        rc = zhaoxi_note_execute(time(NULL), "add", intent.content, &result, reply);
    }
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    else if (intent.kind >= ZHAOXI_LOCAL_DRINK_START && intent.kind <= ZHAOXI_LOCAL_DRINK_STATUS) {
        const char *op = intent.kind == ZHAOXI_LOCAL_DRINK_START ? "start" :
                         intent.kind == ZHAOXI_LOCAL_DRINK_STOP ? "stop" : "status";
        struct zhaoxi_drink_result result;
        rc = zhaoxi_drink_execute(op, 60, message->channel, &result, reply);
        printf("LOCAL_DRINK op=%s rc=%d verified=%d id=%s count=%u removed=%u\n",
               op, rc, result.verified, result.id, result.count, result.removed);
    }
    else if (intent.kind == ZHAOXI_LOCAL_REMIND_IN) {
        struct zhaoxi_reminder_result result;
        rc = zhaoxi_reminder_create(intent.delay_seconds, intent.content, message->channel, &result, reply);
        printf("LOCAL_REMINDER rc=%d verified=%d uncertain=%d id=%s at_epoch=%lld channel=%s\n",
               rc, result.verified, result.storage_uncertain, result.id,
               (long long)result.at_epoch, message->channel);
    }
#endif
    else {
        const char *op = intent.kind == ZHAOXI_LOCAL_TODO_ADD ? "add" :
                         intent.kind == ZHAOXI_LOCAL_TODO_LIST ? "list" : "done";
        struct zhaoxi_todo_result result;
        rc = zhaoxi_todo_execute(AGENT_DATA_DIR "/TASKS.md", op, intent.content, &result, reply);
    }
    printf("LOCAL_INTENT op=%s rc=%d\n", name, rc);
    if (rc && !*reply) *reply = strdup(intent.kind >= ZHAOXI_LOCAL_DRINK_START ?
        "喝水提醒操作未完成，请先查询当前设置。" :
        intent.kind == ZHAOXI_LOCAL_REMIND_IN ?
        "提醒尚未确认保存，请先查询现有提醒，再决定是否重试。" :
        intent.kind == ZHAOXI_LOCAL_NOTE_ADD ?
        "备忘尚未确认保存，请先查询今日备忘，避免重复添加。" :
        "待办操作未完成，请检查编号、文件格式、容量与存储状态。");
    return rc;
}

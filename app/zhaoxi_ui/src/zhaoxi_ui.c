/****************************************************************************
 * zhaoxi_ui.c - 朝夕 AI Life Assistant UI
 * Target: 454x454 square display (BES2800BP + RM69330)
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/boardctl.h>
#include <lvgl/lvgl.h>

LV_FONT_DECLARE(zhaoxi_font_20);
LV_FONT_DECLARE(zhaoxi_font_22);
LV_FONT_DECLARE(zhaoxi_font_24);
LV_FONT_DECLARE(zhaoxi_font_28);

/* ── Screen dimensions ──────────────────────────────────────── */
#define SCREEN_W 454
#define SCREEN_H 454
#define NAV_H    70
#define TILE_H   (SCREEN_H - NAV_H)

/* ── Colors (dark theme) ────────────────────────────────────── */
#define COLOR_BG        lv_color_hex(0x0D1117)
#define COLOR_CARD      lv_color_hex(0x161B22)
#define COLOR_ACCENT    lv_color_hex(0x58A6FF)
#define COLOR_TEXT      lv_color_hex(0xE6EDF3)
#define COLOR_TEXT_DIM  lv_color_hex(0x8B949E)
#define COLOR_GREEN     lv_color_hex(0x3FB950)
#define COLOR_ORANGE    lv_color_hex(0xD29922)

/* ── Data files (primary /data/ai_agent, fallback /data/agent) ─ */
#define DATA_DIR_PRIMARY   "/data/ai_agent"
#define DATA_DIR_FALLBACK  "/data/agent"
#define WEATHER_FILE       DATA_DIR_PRIMARY "/WEATHER.md"
#define WEATHER_FILE_ALT   DATA_DIR_FALLBACK "/WEATHER.md"
#define REMINDER_FILE      DATA_DIR_PRIMARY "/REMINDER.md"
#define REMINDER_FILE_ALT  DATA_DIR_FALLBACK "/REMINDER.md"
#define NET_FILE           DATA_DIR_PRIMARY "/NET_STATUS"
#define NET_FILE_ALT       DATA_DIR_FALLBACK "/NET_STATUS"
#define TASKS_FILE         DATA_DIR_PRIMARY "/TASKS.md"
#define TASKS_FILE_ALT     DATA_DIR_FALLBACK "/TASKS.md"

/* ── Static UI elements ─────────────────────────────────────── */
static lv_obj_t *g_clock_label;
static lv_obj_t *g_date_label;
static lv_obj_t *g_greeting_label;
static lv_obj_t *g_weather_label;
static lv_obj_t *g_status_label;
static lv_obj_t *g_agent_label;
static lv_obj_t *g_task_label;

static lv_obj_t *g_tileview;
static lv_obj_t *g_nav_icon[3];
static lv_obj_t *g_nav_lbl[3];

static lv_obj_t *g_reminder_label;
static lv_obj_t *g_chat_log;
static lv_obj_t *g_chat_ta;
static lv_obj_t *g_chat_kb;

static lv_obj_t *g_set_net_label;
static lv_obj_t *g_set_task_label;
static lv_obj_t *g_set_uptime_label;

static lv_timer_t *g_clock_timer;
static lv_timer_t *g_poll_timer;

static time_t g_app_start;
static uint32_t g_alive_count;

static char g_last_weather[128];
static char g_last_reminder[128];
static char g_last_net[128];
static char g_chat_log_buf[1024];
static int g_last_tasks = -1;

/* ── Forward declarations ───────────────────────────────────── */
static void build_home_tile(lv_obj_t *tile);
static void build_chat_tile(lv_obj_t *tile);
static void build_settings_tile(lv_obj_t *tile);
static void update_nav_highlight(int idx);

/* ── File helper: first non-empty line with fallback ────────── */
static bool read_first_line_from(const char *path, char *buf, size_t buflen)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) return false;

    bool ok = false;
    while (fgets(buf, (int)buflen, fp) != NULL) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n > 0) {
            ok = true;
            break;
        }
    }
    fclose(fp);
    return ok;
}

static bool read_first_line(const char *primary, const char *fallback,
                            char *buf, size_t buflen)
{
    if (read_first_line_from(primary, buf, buflen)) return true;
    if (fallback != NULL && read_first_line_from(fallback, buf, buflen)) return true;
    if (buflen > 0) buf[0] = '\0';
    return false;
}

/* ── Clock update timer ─────────────────────────────────────── */
static void clock_timer_cb(lv_timer_t *timer)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* Time: HH:MM */
    char time_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t->tm_hour, t->tm_min);
    if (g_clock_label) lv_label_set_text(g_clock_label, time_buf);

    /* Date: MM月DD日 星期X */
    static const char *weekdays[] = {
        "日", "一", "二", "三", "四", "五", "六"
    };
    char date_buf[64];
    snprintf(date_buf, sizeof(date_buf), "%d月%d日 周%s",
             t->tm_mon + 1, t->tm_mday, weekdays[t->tm_wday]);
    if (g_date_label) lv_label_set_text(g_date_label, date_buf);

    /* Greeting based on hour */
    const char *greeting;
    if (t->tm_hour < 6) greeting = "夜深了，注意休息";
    else if (t->tm_hour < 9) greeting = "早上好，新的一天开始了";
    else if (t->tm_hour < 12) greeting = "上午好，工作顺利";
    else if (t->tm_hour < 14) greeting = "中午好，记得午休";
    else if (t->tm_hour < 18) greeting = "下午好，继续加油";
    else if (t->tm_hour < 22) greeting = "晚上好，放松一下";
    else greeting = "夜深了，早点休息";
    if (g_greeting_label) lv_label_set_text(g_greeting_label, greeting);
}

/* ── Task count: read TASKS.md, count unfinished "- [ ]" ───── */
static int count_pending_tasks(void)
{
    FILE *fp = fopen(TASKS_FILE, "r");
    if (fp == NULL) {
        /* Older firmware keeps agent data under /data/agent */
        fp = fopen(TASKS_FILE_ALT, "r");
        if (fp == NULL) {
            return 0;
        }
    }

    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "- [ ]", 5) == 0) {
            count++;
        }
    }
    fclose(fp);
    return count;
}

/* ── File-driven state poll timer (every 2 sec) ─────────────── */
static void poll_timer_cb(lv_timer_t *timer)
{
    char buf[128];

    /* Weather */
    if (read_first_line(WEATHER_FILE, WEATHER_FILE_ALT, buf, sizeof(buf))) {
        if (strcmp(buf, g_last_weather) != 0) {
            strncpy(g_last_weather, buf, sizeof(g_last_weather) - 1);
            g_last_weather[sizeof(g_last_weather) - 1] = '\0';
            LV_LOG_USER("DIAG FILE weather=%s", buf);
        }
        if (g_weather_label) {
            lv_label_set_text(g_weather_label, buf);
            lv_obj_set_style_text_color(g_weather_label, COLOR_TEXT, 0);
        }
    } else {
        if (g_weather_label) {
            lv_label_set_text(g_weather_label, "暂无数据");
            lv_obj_set_style_text_color(g_weather_label, COLOR_TEXT_DIM, 0);
        }
    }

    /* Reminder */
    if (read_first_line(REMINDER_FILE, REMINDER_FILE_ALT, buf, sizeof(buf))) {
        if (strcmp(buf, g_last_reminder) != 0) {
            strncpy(g_last_reminder, buf, sizeof(g_last_reminder) - 1);
            g_last_reminder[sizeof(g_last_reminder) - 1] = '\0';
            LV_LOG_USER("DIAG FILE reminder=%s", buf);
        }
        if (g_reminder_label) {
            char rbuf[160];
            snprintf(rbuf, sizeof(rbuf), "提醒：%s", buf);
            lv_label_set_text(g_reminder_label, rbuf);
        }
    } else {
        if (g_reminder_label) lv_label_set_text(g_reminder_label, "提醒：暂无");
    }

    /* Network status */
    if (read_first_line(NET_FILE, NET_FILE_ALT, buf, sizeof(buf))) {
        if (strcmp(buf, g_last_net) != 0) {
            strncpy(g_last_net, buf, sizeof(g_last_net) - 1);
            g_last_net[sizeof(g_last_net) - 1] = '\0';
            LV_LOG_USER("DIAG FILE net=%s", buf);
        }
        char net_buf[128];
        if (strncmp(buf, "CONNECTED", 9) == 0) {
            const char *ip = buf + 9;
            while (*ip == ' ') ip++;
            if (*ip != '\0') {
                snprintf(net_buf, sizeof(net_buf), LV_SYMBOL_WIFI " %s", ip);
            } else {
                snprintf(net_buf, sizeof(net_buf), LV_SYMBOL_WIFI " 已连接");
            }
            if (g_status_label) {
                lv_label_set_text(g_status_label, net_buf);
                lv_obj_set_style_text_color(g_status_label, COLOR_GREEN, 0);
            }
            if (g_set_net_label) {
                lv_label_set_text(g_set_net_label, net_buf);
                lv_obj_set_style_text_color(g_set_net_label, COLOR_GREEN, 0);
            }
        } else {
            if (g_status_label) {
                lv_label_set_text(g_status_label, LV_SYMBOL_WIFI " 未连接");
                lv_obj_set_style_text_color(g_status_label, COLOR_ORANGE, 0);
            }
            if (g_set_net_label) {
                lv_label_set_text(g_set_net_label, "未连接");
                lv_obj_set_style_text_color(g_set_net_label, COLOR_ORANGE, 0);
            }
        }
    }
    /* Absent file: keep last known state, never crash */

    /* Pending tasks */
    int tasks = count_pending_tasks();
    if (tasks != g_last_tasks) {
        g_last_tasks = tasks;
        LV_LOG_USER("DIAG FILE tasks=%d", tasks);
    }
    if (g_task_label) {
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%d 条待办", tasks);
        lv_label_set_text(g_task_label, tbuf);
    }
    if (g_set_task_label) {
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "待办任务：%d 条", tasks);
        lv_label_set_text(g_set_task_label, tbuf);
    }

    /* Uptime (since app start) */
    if (g_set_uptime_label) {
        long up = (long)(time(NULL) - g_app_start);
        if (up < 0) up = 0;
        char ubuf[48];
        snprintf(ubuf, sizeof(ubuf), "运行时间：%ld 分 %ld 秒", up / 60, up % 60);
        lv_label_set_text(g_set_uptime_label, ubuf);
    }
}

/* ── WiFi status label ────────────────────────────────────────
 * NOTE: An earlier revision queried wlan0/eth0 flags via
 * socket()+ioctl(SIOCGIFFLAGS) from this LVGL thread. On BES the
 * network stack lives behind rpmsg on another core, and concurrent
 * socket creation from the UI thread while ai_agent ran TLS sessions
 * caused heap corruption asserts (mm_malloc.c). The check was removed;
 * the label is now fed from the NET_STATUS file instead. ────────── */
static void create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_W, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* WiFi status */
    g_status_label = lv_label_create(bar);
    lv_label_set_text(g_status_label, LV_SYMBOL_WIFI " Connected");
    lv_obj_set_style_text_color(g_status_label, COLOR_GREEN, 0);
    lv_obj_set_style_text_font(g_status_label, &zhaoxi_font_20, 0);
    lv_obj_align(g_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    /* AI Agent status */
    g_agent_label = lv_label_create(bar);
    lv_label_set_text(g_agent_label, LV_SYMBOL_OK " AI Ready");
    lv_obj_set_style_text_color(g_agent_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(g_agent_label, &zhaoxi_font_20, 0);
    lv_obj_align(g_agent_label, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* ── Create main clock area (center) ────────────────────────── */
static void create_clock_area(lv_obj_t *parent)
{
    /* Clock container */
    lv_obj_t *clock_card = lv_obj_create(parent);
    lv_obj_set_size(clock_card, SCREEN_W - 40, 180);
    lv_obj_align(clock_card, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_bg_color(clock_card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(clock_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clock_card, 0, 0);
    lv_obj_set_style_radius(clock_card, 20, 0);
    lv_obj_clear_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);

    /* Time display - large */
    g_clock_label = lv_label_create(clock_card);
    lv_label_set_text(g_clock_label, "00:00");
    lv_obj_set_style_text_color(g_clock_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_clock_label, LV_ALIGN_CENTER, 0, -20);

    /* Date display */
    g_date_label = lv_label_create(clock_card);
    lv_label_set_text(g_date_label, "1月1日 周一");
    lv_obj_set_style_text_color(g_date_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g_date_label, &zhaoxi_font_20, 0);
    lv_obj_align(g_date_label, LV_ALIGN_CENTER, 0, 25);

    /* Greeting */
    g_greeting_label = lv_label_create(parent);
    lv_label_set_text(g_greeting_label, "你好，欢迎使用朝夕");
    lv_obj_set_style_text_color(g_greeting_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(g_greeting_label, &zhaoxi_font_22, 0);
    lv_obj_align(g_greeting_label, LV_ALIGN_TOP_MID, 0, 250);
}

/* ── Create info cards area ─────────────────────────────────── */
static void create_info_cards(lv_obj_t *parent)
{
    /* Weather card */
    lv_obj_t *weather_card = lv_obj_create(parent);
    lv_obj_set_size(weather_card, (SCREEN_W - 52) / 2, 80);
    lv_obj_align(weather_card, LV_ALIGN_TOP_LEFT, 16, 290);
    lv_obj_set_style_bg_color(weather_card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(weather_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(weather_card, 0, 0);
    lv_obj_set_style_radius(weather_card, 16, 0);
    lv_obj_clear_flag(weather_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *weather_title = lv_label_create(weather_card);
    lv_label_set_text(weather_title, LV_SYMBOL_LOOP " 天气");
    lv_obj_set_style_text_color(weather_title, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(weather_title, &zhaoxi_font_20, 0);
    lv_obj_align(weather_title, LV_ALIGN_TOP_LEFT, 4, 4);

    g_weather_label = lv_label_create(weather_card);
    lv_label_set_text(g_weather_label, "暂无数据");
    lv_obj_set_style_text_color(g_weather_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g_weather_label, &zhaoxi_font_28, 0);
    lv_obj_align(g_weather_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    /* Task/Reminder card */
    lv_obj_t *task_card = lv_obj_create(parent);
    lv_obj_set_size(task_card, (SCREEN_W - 52) / 2, 80);
    lv_obj_align(task_card, LV_ALIGN_TOP_RIGHT, -16, 290);
    lv_obj_set_style_bg_color(task_card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(task_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(task_card, 0, 0);
    lv_obj_set_style_radius(task_card, 16, 0);
    lv_obj_clear_flag(task_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *task_title = lv_label_create(task_card);
    lv_label_set_text(task_title, LV_SYMBOL_BELL " 提醒");
    lv_obj_set_style_text_color(task_title, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(task_title, &zhaoxi_font_20, 0);
    lv_obj_align(task_title, LV_ALIGN_TOP_LEFT, 4, 4);

    g_task_label = lv_label_create(task_card);
    lv_label_set_text(g_task_label, "0 条待办");
    lv_obj_set_style_text_color(g_task_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_task_label, &zhaoxi_font_28, 0);
    lv_obj_align(g_task_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

/* ── Home tile ──────────────────────────────────────────────── */
static void build_home_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(tile);
    create_clock_area(tile);
    create_info_cards(tile);
}

/* ── Chat tile (local only, no agent round-trip) ────────────── */
static void chat_send(void)
{
    if (g_chat_ta == NULL) return;

    const char *txt = lv_textarea_get_text(g_chat_ta);
    if (txt != NULL && txt[0] != '\0') {
        LV_LOG_USER("INPUTDBG: %s", txt);

        size_t cur = strlen(g_chat_log_buf);
        size_t add = strlen(txt);
        if (cur + add + 6 < sizeof(g_chat_log_buf)) {
            strcat(g_chat_log_buf, "\n> ");
            strcat(g_chat_log_buf, txt);
        }
        if (g_chat_log) lv_label_set_text(g_chat_log, g_chat_log_buf);
        lv_textarea_set_text(g_chat_ta, "");
    }
}

static void chat_ready_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_READY) return;
    chat_send();
}

static void chat_send_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    chat_send();
}

static void build_chat_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* Reminder line */
    g_reminder_label = lv_label_create(tile);
    lv_label_set_text(g_reminder_label, "提醒：暂无");
    lv_obj_set_style_text_color(g_reminder_label, COLOR_ORANGE, 0);
    lv_obj_set_style_text_font(g_reminder_label, &zhaoxi_font_20, 0);
    lv_obj_set_width(g_reminder_label, SCREEN_W - 20);
    lv_label_set_long_mode(g_reminder_label, LV_LABEL_LONG_DOT);
    lv_obj_align(g_reminder_label, LV_ALIGN_TOP_LEFT, 10, 4);

    /* Text area */
    g_chat_ta = lv_textarea_create(tile);
    lv_obj_set_size(g_chat_ta, SCREEN_W - 110, 44);
    lv_obj_align(g_chat_ta, LV_ALIGN_TOP_LEFT, 10, 34);
    lv_textarea_set_one_line(g_chat_ta, true);
    lv_textarea_set_placeholder_text(g_chat_ta, "输入内容...");
    lv_obj_set_style_text_font(g_chat_ta, &zhaoxi_font_20, 0);
    lv_obj_add_event_cb(g_chat_ta, chat_ready_cb, LV_EVENT_READY, NULL);

    /* Send button */
    lv_obj_t *send = lv_btn_create(tile);
    lv_obj_set_size(send, 80, 44);
    lv_obj_align(send, LV_ALIGN_TOP_RIGHT, -10, 34);
    lv_obj_set_style_bg_color(send, COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_add_event_cb(send, chat_send_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *send_lbl = lv_label_create(send);
    lv_label_set_text(send_lbl, "发送");
    lv_obj_set_style_text_color(send_lbl, COLOR_BG, 0);
    lv_obj_set_style_text_font(send_lbl, &zhaoxi_font_20, 0);
    lv_obj_align(send_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Log */
    strncpy(g_chat_log_buf, "对话记录：", sizeof(g_chat_log_buf) - 1);
    g_chat_log = lv_label_create(tile);
    lv_obj_set_size(g_chat_log, SCREEN_W - 20, 62);
    lv_obj_align(g_chat_log, LV_ALIGN_TOP_LEFT, 10, 88);
    lv_label_set_long_mode(g_chat_log, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_chat_log, g_chat_log_buf);
    lv_obj_set_style_text_color(g_chat_log, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_chat_log, &zhaoxi_font_20, 0);

    /* Keyboard (sized so it does not cover the log) */
    g_chat_kb = lv_keyboard_create(tile);
    lv_obj_set_size(g_chat_kb, SCREEN_W, 220);
    lv_obj_align(g_chat_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(g_chat_kb, g_chat_ta);
}

/* ── Settings tile (read-only info) ─────────────────────────── */
static void build_settings_tile(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, SCREEN_W - 32, 250);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " 设备信息");
    lv_obj_set_style_text_color(title, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(title, &zhaoxi_font_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 0);

    g_set_net_label = lv_label_create(card);
    lv_label_set_text(g_set_net_label, "网络：未知");
    lv_obj_set_style_text_color(g_set_net_label, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(g_set_net_label, &zhaoxi_font_20, 0);
    lv_obj_align(g_set_net_label, LV_ALIGN_TOP_LEFT, 4, 50);

    g_set_task_label = lv_label_create(card);
    lv_label_set_text(g_set_task_label, "待办任务：0 条");
    lv_obj_set_style_text_color(g_set_task_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_set_task_label, &zhaoxi_font_20, 0);
    lv_obj_align(g_set_task_label, LV_ALIGN_TOP_LEFT, 4, 90);

    g_set_uptime_label = lv_label_create(card);
    lv_label_set_text(g_set_uptime_label, "运行时间：0 分 0 秒");
    lv_obj_set_style_text_color(g_set_uptime_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_set_uptime_label, &zhaoxi_font_20, 0);
    lv_obj_align(g_set_uptime_label, LV_ALIGN_TOP_LEFT, 4, 130);

    lv_obj_t *note = lv_label_create(card);
    lv_label_set_text(note, "朝夕 AI 助手 · 演示版");
    lv_obj_set_style_text_color(note, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(note, &zhaoxi_font_20, 0);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 4, 180);
}

/* ── Nav highlight + change-only diagnostic ─────────────────── */
static void update_nav_highlight(int idx)
{
    for (int i = 0; i < 3; i++) {
        lv_color_t c = (i == idx) ? COLOR_ACCENT : COLOR_TEXT_DIM;
        if (g_nav_icon[i]) lv_obj_set_style_text_color(g_nav_icon[i], c, 0);
        if (g_nav_lbl[i]) lv_obj_set_style_text_color(g_nav_lbl[i], c, 0);
    }
}

static void nav_set_active(int idx)
{
    static int last_idx = -1;
    update_nav_highlight(idx);
    if (idx != last_idx) {
        last_idx = idx;
        LV_LOG_USER("DIAG NAV idx=%d", idx);
    }
}

/* ── Tileview events: swipe keeps nav highlight in sync ─────── */
static void tileview_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *tile = g_tileview ? lv_tileview_get_tile_active(g_tileview) : NULL;
    nav_set_active(tile ? (int)lv_obj_get_index(tile) : 0);
}

/* ── Touch diagnostics: indev-level, cannot swallow clicks ──── */
static void indev_diag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) return;

    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(e);
    lv_point_t p = {0, 0};
    if (indev) lv_indev_get_point(indev, &p);
    LV_LOG_USER("DIAG TOUCH x=%d y=%d state=%s", (int)p.x, (int)p.y,
                code == LV_EVENT_PRESSED ? "pressed" : "released");
}

/* ── Create bottom nav bar ──────────────────────────────────── */
static void nav_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_tileview) {
        lv_tileview_set_tile_by_index(g_tileview, (uint32_t)idx, 0, LV_ANIM_ON);
    }
    nav_set_active(idx);
}

static void create_nav_bar(lv_obj_t *parent)
{
    lv_obj_t *nav = lv_obj_create(parent);
    lv_obj_set_size(nav, SCREEN_W, NAV_H);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    static const char *icons[] = {
        LV_SYMBOL_HOME,
        LV_SYMBOL_LIST,
        LV_SYMBOL_SETTINGS,
    };
    static const char *labels[] = { "首页", "对话", "设置" };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(nav);
        lv_obj_set_size(btn, 80, 56);
        lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, nav_btn_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        g_nav_icon[i] = lv_label_create(btn);
        lv_label_set_text(g_nav_icon[i], icons[i]);
        lv_obj_set_style_text_color(g_nav_icon[i], i == 0 ? COLOR_ACCENT : COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(g_nav_icon[i], &zhaoxi_font_24, 0);
        lv_obj_align(g_nav_icon[i], LV_ALIGN_CENTER, 0, -8);

        g_nav_lbl[i] = lv_label_create(btn);
        lv_label_set_text(g_nav_lbl[i], labels[i]);
        lv_obj_set_style_text_color(g_nav_lbl[i], i == 0 ? COLOR_ACCENT : COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(g_nav_lbl[i], &zhaoxi_font_20, 0);
        lv_obj_align(g_nav_lbl[i], LV_ALIGN_CENTER, 0, 14);
    }
}

/* ── Main entry ─────────────────────────────────────────────── */
int main(int argc, FAR char *argv[])
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;

    if (lv_is_initialized()) {
        LV_LOG_ERROR("LVGL already initialized!");
        return -1;
    }

#ifdef CONFIG_BOARDCTL
    boardctl(BOARDIOC_INIT, 0);
#endif

    lv_init();
    lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
    info.fb_path = "/dev/lcd0";
#endif
#ifdef CONFIG_INPUT_TOUCHSCREEN
    info.input_path = "/dev/input0";
#endif

    lv_nuttx_init(&info, &result);
    if (result.disp == NULL) {
        LV_LOG_ERROR("Display init failed!");
        return 1;
    }

    g_app_start = time(NULL);

    /* Touch diagnostics at the input-device level (does not eat clicks) */
    if (result.indev) {
        lv_indev_add_event_cb(result.indev, indev_diag_cb, LV_EVENT_ALL, NULL);
    }

    /* Set dark theme */
    lv_display_set_default(result.disp);
    lv_theme_t *th = lv_theme_default_init(result.disp,
        COLOR_ACCENT, COLOR_ACCENT,
        true, LV_FONT_DEFAULT);
    lv_display_set_theme(result.disp, th);

    /* Create main screen */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Tileview fills the area above the nav bar */
    g_tileview = lv_tileview_create(scr);
    lv_obj_set_size(g_tileview, SCREEN_W, TILE_H);
    lv_obj_align(g_tileview, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(g_tileview, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_tileview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_tileview, 0, 0);
    lv_obj_set_style_radius(g_tileview, 0, 0);
    lv_obj_set_style_pad_all(g_tileview, 0, 0);
    lv_obj_set_scrollbar_mode(g_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_tileview, tileview_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *tile_home = lv_tileview_add_tile(g_tileview, 0, 0, LV_DIR_HOR);
    lv_obj_t *tile_chat = lv_tileview_add_tile(g_tileview, 1, 0, LV_DIR_HOR);
    lv_obj_t *tile_set  = lv_tileview_add_tile(g_tileview, 2, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(tile_home, 0, 0);
    lv_obj_set_style_pad_all(tile_chat, 0, 0);
    lv_obj_set_style_pad_all(tile_set, 0, 0);

    /* Build the three tiles once */
    build_home_tile(tile_home);
    build_chat_tile(tile_chat);
    build_settings_tile(tile_set);

    /* Persistent nav bar (sibling, below tileview) */
    create_nav_bar(scr);
    nav_set_active(0);

    /* Start clock update timer (every 1 sec) */
    g_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(g_clock_timer); /* initial update */

    /* Start file poll timer (every 2 sec) */
    g_poll_timer = lv_timer_create(poll_timer_cb, 2000, NULL);
    poll_timer_cb(g_poll_timer); /* initial update */

    /* Startup diagnostic summary */
    {
        char dbuf[128];
        bool w_ok = read_first_line(WEATHER_FILE, WEATHER_FILE_ALT, dbuf, sizeof(dbuf));
        bool r_ok = read_first_line(REMINDER_FILE, REMINDER_FILE_ALT, dbuf, sizeof(dbuf));
        bool n_ok = read_first_line(NET_FILE, NET_FILE_ALT, dbuf, sizeof(dbuf));
        int n_tasks = count_pending_tasks();
        LV_LOG_USER("DIAG READY weather=%s reminder=%s net=%s tasks=%d fonts=4",
                    w_ok ? "ok" : "none", r_ok ? "ok" : "none",
                    n_ok ? "ok" : "none", n_tasks);
    }

    LV_LOG_USER("Zhaoxi UI started!");

    /* Main event loop */
    time_t last_alive = time(NULL);
    while (1) {
        uint32_t idle = lv_timer_handler();
        idle = idle ? idle : 1;
        usleep(idle * 1000);

        time_t now = time(NULL);
        if (now - last_alive >= 30) {
            last_alive = now;
            g_alive_count++;
            LV_LOG_USER("DIAG ALIVE %u", (unsigned)g_alive_count);
        }
    }

    lv_nuttx_deinit(&result);
    lv_deinit();
    return 0;
}

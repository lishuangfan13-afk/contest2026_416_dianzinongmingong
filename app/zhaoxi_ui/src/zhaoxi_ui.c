/****************************************************************************
 * zhaoxi_ui.c - 朝夕 AI Life Assistant UI
 * Target: 454x454 square display (BES2800BP + RM69330)
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <sys/boardctl.h>
#include <lvgl/lvgl.h>

/* ── Screen dimensions ──────────────────────────────────────── */
#define SCREEN_W 454
#define SCREEN_H 454

/* ── Colors (dark theme) ────────────────────────────────────── */
#define COLOR_BG        lv_color_hex(0x0D1117)
#define COLOR_CARD      lv_color_hex(0x161B22)
#define COLOR_ACCENT    lv_color_hex(0x58A6FF)
#define COLOR_TEXT      lv_color_hex(0xE6EDF3)
#define COLOR_TEXT_DIM  lv_color_hex(0x8B949E)
#define COLOR_GREEN     lv_color_hex(0x3FB950)
#define COLOR_ORANGE    lv_color_hex(0xD29922)

/* ── Static UI elements ─────────────────────────────────────── */
static lv_obj_t *g_clock_label;
static lv_obj_t *g_date_label;
static lv_obj_t *g_greeting_label;
static lv_obj_t *g_weather_label;
static lv_obj_t *g_status_label;
static lv_obj_t *g_agent_label;
static lv_timer_t *g_clock_timer;

/* ── Clock update timer ─────────────────────────────────────── */
static void clock_timer_cb(lv_timer_t *timer)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* Time: HH:MM */
    char time_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t->tm_hour, t->tm_min);
    lv_label_set_text(g_clock_label, time_buf);

    /* Date: MM月DD日 星期X */
    static const char *weekdays[] = {
        "日", "一", "二", "三", "四", "五", "六"
    };
    char date_buf[64];
    snprintf(date_buf, sizeof(date_buf), "%d月%d日 周%s",
             t->tm_mon + 1, t->tm_mday, weekdays[t->tm_wday]);
    lv_label_set_text(g_date_label, date_buf);

    /* Greeting based on hour */
    const char *greeting;
    if (t->tm_hour < 6) greeting = "夜深了，注意休息";
    else if (t->tm_hour < 9) greeting = "早上好，新的一天开始了";
    else if (t->tm_hour < 12) greeting = "上午好，工作顺利";
    else if (t->tm_hour < 14) greeting = "中午好，记得午休";
    else if (t->tm_hour < 18) greeting = "下午好，继续加油";
    else if (t->tm_hour < 22) greeting = "晚上好，放松一下";
    else greeting = "夜深了，早点休息";
    lv_label_set_text(g_greeting_label, greeting);
}

/* ── Create status bar (top) ────────────────────────────────── */
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
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_20, 0);
    lv_obj_align(g_status_label, LV_ALIGN_LEFT_MID, 0, 0);

    /* AI Agent status */
    g_agent_label = lv_label_create(bar);
    lv_label_set_text(g_agent_label, LV_SYMBOL_OK " AI Ready");
    lv_obj_set_style_text_color(g_agent_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(g_agent_label, &lv_font_montserrat_20, 0);
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
    lv_obj_set_style_text_font(g_date_label, &lv_font_montserrat_20, 0);
    lv_obj_align(g_date_label, LV_ALIGN_CENTER, 0, 25);

    /* Greeting */
    g_greeting_label = lv_label_create(parent);
    lv_label_set_text(g_greeting_label, "你好，欢迎使用朝夕");
    lv_obj_set_style_text_color(g_greeting_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(g_greeting_label, &lv_font_montserrat_22, 0);
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
    lv_obj_set_style_text_font(weather_title, &lv_font_montserrat_20, 0);
    lv_obj_align(weather_title, LV_ALIGN_TOP_LEFT, 4, 4);

    g_weather_label = lv_label_create(weather_card);
    lv_label_set_text(g_weather_label, "-- C");
    lv_obj_set_style_text_color(g_weather_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_weather_label, &lv_font_montserrat_28, 0);
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
    lv_obj_set_style_text_font(task_title, &lv_font_montserrat_20, 0);
    lv_obj_align(task_title, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t *task_value = lv_label_create(task_card);
    lv_label_set_text(task_value, "0 条待办");
    lv_obj_set_style_text_color(task_value, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(task_value, &lv_font_montserrat_28, 0);
    lv_obj_align(task_value, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

/* ── Create bottom nav bar ──────────────────────────────────── */
static void nav_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    /* TODO: switch to different screens */
    LV_LOG_USER("Nav button %d clicked", idx);
}

static void create_nav_bar(lv_obj_t *parent)
{
    lv_obj_t *nav = lv_obj_create(parent);
    lv_obj_set_size(nav, SCREEN_W, 70);
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

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_color(icon, i == 0 ? COLOR_ACCENT : COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -8);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_color(lbl, i == 0 ? COLOR_ACCENT : COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 14);
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

    /* Build UI components */
    create_status_bar(scr);
    create_clock_area(scr);
    create_info_cards(scr);
    create_nav_bar(scr);

    /* Start clock update timer (every 1 sec) */
    g_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(g_clock_timer); /* initial update */

    LV_LOG_USER("Zhaoxi UI started!");

    /* Main event loop */
    while (1) {
        uint32_t idle = lv_timer_handler();
        idle = idle ? idle : 1;
        usleep(idle * 1000);
    }

    lv_nuttx_deinit(&result);
    lv_deinit();
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MORNING_PROFILE_MAX 1024
#define MORNING_ITEMS_MAX 5
#define MORNING_JSON_MAX 32768
struct morning_item {
    char id[65], title[385], source[129], url[769];
    char summary[769], relevance[385];
    int64_t published_at;
};
struct morning_batch {
    char date[11], id[17];
    unsigned count, played;
    bool asked, declined;
    struct morning_item items[MORNING_ITEMS_MAX];
};
int morning_day(int64_t epoch, char date[11], unsigned *minute);
int morning_profile_get(char *out, size_t cap);
int morning_profile_set(const char *text);
int morning_batch_parse(const char *json, int64_t now, struct morning_batch *out);
int morning_batch_load(const char *date, struct morning_batch *out);
int morning_batch_render(const struct morning_batch *batch, char *out, size_t cap);
int morning_batch_save(const struct morning_batch *batch);
int morning_batch_publish(const struct morning_batch *batch);
/* Refresh appends new IDs, preserving existing items and playback progress. */
int morning_batch_refresh(const struct morning_batch *batch);
int morning_item_seen(const char *id, const char *today);
int morning_batch_prune(int64_t now);
int morning_consent_parse(const char *text); /* +1 yes, -1 no, 0 unclear */
bool morning_play_request(const char *text);
/* -1: not a playback request; 0: today; 1: yesterday. */
int morning_play_day_request(const char *text);
bool morning_due(int64_t epoch, bool prepared); /* 06:55 prefetch, catch up before 22:00 */
/* Runtime hooks, owned by the long-lived Agent task group. */
int zhaoxi_morning_start(void);
int zhaoxi_morning_status(char *out, size_t cap);
int zhaoxi_morning_sync_time(int64_t epoch, const char *source);
int zhaoxi_morning_command(int argc, char **argv);
int zhaoxi_morning_voice(const char *text, bool gated);
int zhaoxi_morning_import(const char *json);
int zhaoxi_morning_set_profile(const char *text);

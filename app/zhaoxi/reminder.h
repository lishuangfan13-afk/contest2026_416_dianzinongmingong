/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
struct zhaoxi_reminder_result {
    char id[9];
    int64_t at_epoch;
    int verified;
    int storage_uncertain;
};
int zhaoxi_reminder_create(unsigned delay, const char *text, const char *channel,
                          struct zhaoxi_reminder_result *result, char **reply);

int zhaoxi_play_alert(void);
int zhaoxi_deliver_reminder(const char *text);
int zhaoxi_reminder_command(int argc, char **argv, int at, const char *channel);

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

int zhaoxi_play_alert(void);
int zhaoxi_deliver_reminder(const char *text);
int zhaoxi_reminder_command(int argc, char **argv, int at, const char *channel);

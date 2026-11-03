/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Agent calls after local infrastructure initialization and before any
 * network watcher, normal automatic ASR, cron, or news service starts. */
int zhaoxi_uplink_boot_dialog(bool pc_owned);
void zhaoxi_uplink_boot_stop(void);
bool zhaoxi_uplink_boot_used(void);
int zhaoxi_uplink_boot_status(char *out,size_t capacity);

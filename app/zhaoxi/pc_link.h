/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
int zhaoxi_pc_link(unsigned seconds);
bool zhaoxi_pc_link_stopping(void);
bool zhaoxi_pc_link_alive(void);
bool zhaoxi_pc_link_ready(void);
void zhaoxi_pc_link_request_stop(void);
int zhaoxi_pc_link_boot(void);

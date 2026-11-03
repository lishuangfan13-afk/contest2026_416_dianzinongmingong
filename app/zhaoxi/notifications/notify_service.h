/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
int zhaoxi_notify_start(void);
int zhaoxi_notify_status(char *out,size_t cap);
/* No network IO in callers; background delivery uses the confirmed target. */
int zhaoxi_notify_reminder_event(const char *job_id,int64_t due,const char *text);
int zhaoxi_notify_news(unsigned previous_days);
int zhaoxi_notify_todos(void);
/* Finite statement grammar: 1=today news, 2=yesterday news, 3=todo snapshot. */
int zhaoxi_notify_request(const char *text);

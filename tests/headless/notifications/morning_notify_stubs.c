/* SPDX-License-Identifier: Apache-2.0 */
/* Existing morning lifecycle regressions exercise notifications disabled. */
#include <errno.h>
int zhaoxi_notify_start(void){return 0;}
int zhaoxi_notify_news(unsigned offset){(void)offset;return -EACCES;}
int zhaoxi_notify_todos(void){return -EACCES;}

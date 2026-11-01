/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZHAOXI_DRINK_H
#define ZHAOXI_DRINK_H
#include <stdint.h>
struct zhaoxi_drink_result {
    char id[9];
    unsigned interval_s;
    int64_t next_epoch;
    unsigned count;
    unsigned removed;
    int existing;
    int enabled;
    int verified;
};
/* Cron must be running. The caller owns any returned reply, including errors.
 * This operation never submits outbound messages; start requires 1..1440 min. */
int zhaoxi_drink_execute(const char *operation, unsigned minutes, const char *channel,
                         struct zhaoxi_drink_result *result, char **reply);
int zhaoxi_drink_command(int argc, char **argv, int at, const char *channel);
#endif

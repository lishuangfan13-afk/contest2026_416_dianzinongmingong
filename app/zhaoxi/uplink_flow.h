/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
struct uplink_flow_io {
    void *context;
    int (*prompt)(void *,const char *);
    int (*listen)(void *,int *label);
    int (*connect)(void *,int label);
    int (*cancelled)(void *);
};
/* Only labels 1 (wired) and 2 (Wi-Fi) can initiate a connection. Unknown,
 * silence and capture errors ask again. Network failures announce and retry. */
int uplink_flow_run(const struct uplink_flow_io *io);

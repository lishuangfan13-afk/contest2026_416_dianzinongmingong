/* SPDX-License-Identifier: Apache-2.0 */
#include "uplink_flow.h"
#include <errno.h>
int uplink_flow_run(const struct uplink_flow_io *io)
{
    if(!io || !io->prompt || !io->listen || !io->connect || !io->cancelled)return -EINVAL;
    while(!io->cancelled(io->context)) {
        int rc=io->prompt(io->context,"uplink_choose");if(rc)return rc;
        if(io->cancelled(io->context))break;
        int label=0;rc=io->listen(io->context,&label);
        if(io->cancelled(io->context))break;
        if(rc || (label!=1 && label!=2))continue;
        rc=io->connect(io->context,label);
        if(io->cancelled(io->context))break;
        if(!rc)return io->prompt(io->context,"uplink_ok");
        rc=io->prompt(io->context,"uplink_fail");if(rc)return rc;
    }
    return -ECANCELED;
}

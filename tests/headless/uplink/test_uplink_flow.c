/* SPDX-License-Identifier: Apache-2.0 */
#include "uplink_flow.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fake {unsigned step,prompts,connections;int stopped,broken_prompt;};
static int prompt(void *ctx,const char *id)
{
    struct fake *f=ctx;
    const char *expected[]={"uplink_choose","uplink_choose","uplink_fail","uplink_choose","uplink_ok"};
    assert(f->prompts<5 && !strcmp(id,expected[f->prompts++]));
    return f->broken_prompt?-EIO:0;
}
static int listen(void *ctx,int *label)
{
    struct fake *f=ctx;
    int labels[]={0,2,1};assert(f->step<3);*label=labels[f->step++];return 0;
}
static int connect(void *ctx,int label)
{
    struct fake *f=ctx;
    assert(label==(f->connections?1:2));f->connections++;return label==2?-ENETDOWN:0;
}
static int cancelled(void *ctx){return ((struct fake *)ctx)->stopped;}
int main(void)
{
    struct fake f={0};struct uplink_flow_io io={&f,prompt,listen,connect,cancelled};
    assert(!uplink_flow_run(&io));assert(f.prompts==5 && f.connections==2);
    f=(struct fake){.stopped=1};assert(uplink_flow_run(&io)==-ECANCELED);assert(!f.prompts && !f.connections);
    f=(struct fake){.broken_prompt=1};assert(uplink_flow_run(&io)==-EIO);assert(!f.connections);
    assert(uplink_flow_run(NULL)==-EINVAL);
    puts("PASS: prompt-before-listen, unknown never connects, failure/reask, chosen-mode-only, success and cancel");
    return 0;
}

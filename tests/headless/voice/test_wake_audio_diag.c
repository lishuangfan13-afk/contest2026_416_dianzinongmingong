/* SPDX-License-Identifier: Apache-2.0 */
#include "wake_audio_diag.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
int main(void)
{
    uint16_t pcm[600];for(unsigned i=0;i<600;i++)pcm[i]=(uint16_t)(i*17U);
    assert(!zhaoxi_wake_audio_command("clear"));
    zhaoxi_wake_audio_capture(pcm,sizeof(pcm));assert(zhaoxi_wake_audio_command("dump")==-ENODATA);
    assert(!zhaoxi_wake_audio_command("arm"));
    zhaoxi_wake_audio_capture(pcm,384002);zhaoxi_wake_audio_capture(pcm,3);
    zhaoxi_wake_audio_capture(pcm,sizeof(pcm));
    zhaoxi_wake_audio_capture(pcm,2); /* One-shot capture never overwrites the first clip. */
    assert(!zhaoxi_wake_audio_command("dump"));
    assert(!zhaoxi_wake_audio_command("clear"));assert(zhaoxi_wake_audio_command("dump")==-ENODATA);
    assert(zhaoxi_wake_audio_command("bad")==-EINVAL);
    return 0;
}

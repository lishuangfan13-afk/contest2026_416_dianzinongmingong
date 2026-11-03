/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
void zhaoxi_wake_audio_capture(const void *pcm,size_t bytes);
int zhaoxi_wake_audio_command(const char *op);

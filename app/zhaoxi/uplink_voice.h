/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "uplink_keywords.h"
#include <stdbool.h>
int uplink_local_prompt(const char *id,bool cancellable);
int uplink_voice_capture(struct uplink_kw_features *out,bool cancellable,unsigned listen_seconds);
int uplink_voice_load(struct uplink_kw_model *out);
int uplink_voice_save(const struct uplink_kw_model *model);
/* argv[0] is uplink-voice; manual mutation/capture requires a stopped Agent. */
int uplink_voice_command(int argc,char **argv);

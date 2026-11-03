/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifndef ZHAOXI_AUDIO_DIR
#define ZHAOXI_AUDIO_DIR "/emmc/zhaoxi/audio"
#endif
#ifndef ZHAOXI_PROMPT_DIR
#define ZHAOXI_PROMPT_DIR ZHAOXI_AUDIO_DIR "/prompts"
#endif
#ifndef ZHAOXI_PROMPT_CONSOLE
#define ZHAOXI_PROMPT_CONSOLE "/dev/console"
#endif
#define ZHAOXI_NEWS_PROMPT "有新的资讯，需要播放吗？请说播放，或暂不播放。"
#define ZHAOXI_NETWORK_PROMPT "网络暂时不可用，请稍后再试。"
#define ZHAOXI_CONNECTED_PROMPT "联网成功"
typedef int (*zhaoxi_prompt_emit)(const unsigned char *, size_t, void *);
typedef int (*zhaoxi_prompt_cancel)(void *);
const char *zhaoxi_prompt_text(const char *id);
const char *zhaoxi_prompt_id(const char *text);
/* handled becomes true only after the entire file passes validation. Once
 * true, callers must not fall back to TTS, including on playback failure. */
int zhaoxi_prompt_stream(const char *id, zhaoxi_prompt_emit emit,
    zhaoxi_prompt_cancel cancel, void *user, bool *handled);
int zhaoxi_prompt_check(const char *id, size_t *bytes);
int zhaoxi_prompt_status(char *out, size_t capacity);
int zhaoxi_prompt_commit(const char *id);
int zhaoxi_prompt_receive(const char *id, size_t length);

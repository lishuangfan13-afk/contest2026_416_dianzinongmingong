/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
/* 1 append, 2 replace, 0 not an explicit profile request. */
int morning_profile_request(const char *text,char *content,size_t capacity);
/* Strict explicit confirmation; a news "play" command is not confirmation. */
int morning_profile_decision(const char *text);

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
int morning_endpoint_valid(const char *endpoint);
int morning_fetch_candidates(const char *endpoint, const char *profile, int64_t now, char *out, size_t cap);
int morning_summarize(const char *profile, const char *candidates, int64_t now, char *out, size_t cap);
int morning_sntp(int64_t *epoch);

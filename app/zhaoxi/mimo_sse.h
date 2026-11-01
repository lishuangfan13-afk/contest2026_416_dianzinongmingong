/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>

typedef struct mimo_sse mimo_sse_t;
/* PCM16LE mono, 24 kHz. All calls contain a whole number of samples. */
typedef int (*mimo_pcm_sink_t)(const unsigned char *, size_t, void *);
mimo_sse_t *mimo_sse_create(mimo_pcm_sink_t sink, void *user);
int mimo_sse_feed(mimo_sse_t *, const unsigned char *, size_t);
int mimo_sse_finish(mimo_sse_t *);
size_t mimo_sse_pcm_bytes(const mimo_sse_t *);
void mimo_sse_destroy(mimo_sse_t *);

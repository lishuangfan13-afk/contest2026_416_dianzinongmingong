/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
typedef struct { const char *name; const char *value; } vela_header_t;
int vela_https_post_json(const char *host, const char *port, const char *path,
                        const vela_header_t *headers, const char *body,
                        char *response, size_t capacity);
int vela_https_post_json_bounded(const char *host,const char *port,const char *path,
    const vela_header_t *headers,const char *body,char *out,size_t capacity,
    unsigned budget_ms,int (*cancel)(void *),void *user);

#!/usr/bin/env python3
"""Exercise the actual TLS pool/request control flow with two FD namespaces.

The SDK's pool and request functions are compiled unchanged. Network/TLS calls
are test doubles modelling NuttX task-group-local FD numbers; no board IO or
cryptographic handshake is performed.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#define LOG_ERR 3
#define LOG_DEBUG 7
#define LOG_INFO 6
#define LOG_WARNING 4
#define syslog(...) ((void)0)
#define CONN_POOL_SIZE 2
#define AGENT_LLM_SOCKET_TIMEOUT_SEC 120
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY -100
#define VELA_TLS_ERR_OVERFLOW -101
typedef struct { int fd; } net_t;
typedef struct { int owner; } ssl_t;
typedef struct { ssl_t ssl; net_t net; } tls_ctx_t;
typedef struct { const char *name, *value; } vela_header_t;
static const char *TAG = "test";
static int group, descriptors[2][16], opened, closed, wrong_group, failure;
static void tls_ctx_free(tls_ctx_t *c)
{
    if (c->net.fd >= 0) {
        if (c->ssl.owner != group || descriptors[group][c->net.fd] != 1) wrong_group++;
        descriptors[group][c->net.fd] = 0; c->net.fd = -1; closed++;
    }
}
static int tls_ctx_connect(tls_ctx_t *c, const char *h, const char *p)
{
    (void)h; (void)p; c->net.fd = -1;
    if (failure == 1) return -11;
    c->ssl.owner = group; c->net.fd = 7; descriptors[group][7] = 1; opened++; return 0;
}
static int tls_write_request(tls_ctx_t *c, const char *m, const char *h,
                            const char *p, const vela_header_t *v, const char *b, size_t n)
{
    (void)m; (void)h; (void)p; (void)v; (void)b; (void)n;
    if (c->ssl.owner != group || descriptors[group][c->net.fd] != 1) { wrong_group++; return -12; }
    return failure == 2 ? -13 : 0;
}
static int tls_read_response(tls_ctx_t *c, char *b, size_t cap, size_t *n, bool *keep, bool head_only)
{
    (void)head_only;
    assert(c->ssl.owner == group);
    if (failure == 3) return -14;
    snprintf(b, cap, "ok"); if (n) *n = 2; if (keep) *keep = true; return 200;
}
static int mbedtls_ssl_read(ssl_t *ssl, unsigned char *b, size_t n)
{ (void)b; (void)n; if (ssl->owner != group) wrong_group++; return -15; }
static int fake_setsockopt(int f, int l, int o, const void *v, socklen_t s)
{ (void)f; (void)l; (void)o; (void)v; (void)s; return 0; }
#define setsockopt fake_setsockopt

ACTUAL_POOL
ACTUAL_REQUEST

static int ask(void)
{ char out[64]; return vela_https_request("same.example", "443", "POST", "/", NULL, "x", 1, out, sizeof(out), NULL); }
int main(void)
{
    (void)TAG;
#ifdef CONFIG_EXAMPLES_ZHAOXI
    for (int i = 0; i < 6; i++) {
        group = i % 2;
        /* The other task group has a listener with the same numeric FD. */
        descriptors[1 - group][7] = 2;
        assert(ask() == 200);
        assert(descriptors[group][7] == 0);
        assert(descriptors[1 - group][7] == 2);
    }
    assert(opened == 6 && closed == 6 && wrong_group == 0);
    for (failure = 1; failure <= 3; failure++) {
        int before = opened - closed;
        assert(ask() < 0);
        assert(opened - closed == before && wrong_group == 0);
    }
    vela_tls_pool_cleanup();
    assert(wrong_group == 0 && opened == closed);
    puts("product: alternating FD namespaces stay isolated; connect/write/read failures release resources");
#else
    group = 0; assert(ask() == 200); assert(ask() == 200);
    assert(opened == 1 && closed == 0 && wrong_group == 0);
    vela_tls_pool_cleanup(); assert(closed == 1);
    puts("standard: same-task keep-alive pooling preserved");
#endif
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root', type=Path, required=True)
    parser.add_argument('--expect-broken', action='store_true')
    args = parser.parse_args()
    text = (args.agent_root / 'src/infra/vela_tls.c').read_text()
    start = text.index('typedef struct {', text.index('/* ── Persistent connection pool'))
    end = text.index('#ifdef CONFIG_ZHAOXI_BES_AUDIO', start)
    pool = text[start:end]
    if args.expect_broken:
        start = pool.index('#ifdef CONFIG_EXAMPLES_ZHAOXI', pool.index('static conn_slot_t* pool_acquire'))
        middle = pool.index('#else\n', start)
        end = pool.index('#endif', middle)
        pool = pool[:start] + pool[middle + len('#else\n'):end] + pool[end + len('#endif'):]
    start = text.index('int vela_https_request(')
    request = text[start:text.index('\nint vela_https_get(', start)]
    code = HARNESS.replace('ACTUAL_POOL', pool).replace('ACTUAL_REQUEST', request)
    with tempfile.TemporaryDirectory(prefix='zhaoxi-tls-groups-') as tmp:
        root = Path(tmp); source = root / 'test.c'; source.write_text(code)
        for product in ([True] if args.expect_broken else [True, False]):
            target = root / ('product' if product else 'standard')
            cmd = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if product: cmd += ['-DCONFIG_EXAMPLES_ZHAOXI=1']
            subprocess.run(cmd + [str(source), '-pthread', '-o', str(target)], check=True)
            result = subprocess.run([str(target)], capture_output=True, text=True, timeout=5)
            if args.expect_broken:
                if result.returncode == 0 or 'Assertion' not in result.stderr:
                    raise RuntimeError('Cross-group failure not reproduced:\n' + result.stdout + result.stderr)
                print('before fix: a pooled numeric FD is reused in another task group')
            elif result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            else: print(result.stdout.strip())


if __name__ == '__main__':
    main()

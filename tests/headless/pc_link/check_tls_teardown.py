#!/usr/bin/env python3
"""Exercise the actual SDK cleanup function against a full real socket buffer.

TLS object deallocation and close_notify framing are test doubles. The latter
uses a real write(2), matching the shipped mbedtls_net_send transport behavior.
This tests cleanup boundedness, not a complete TLS handshake or board shutdown.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct { int fd; } net_t;
typedef struct { net_t *net; } ssl_t;
typedef struct { ssl_t ssl; int cfg; net_t net; int ctr_drbg; } tls_ctx_t;
static int notify_calls, releases, nonblock_calls;
static bool inject_nonblock_failure;
static int mbedtls_net_set_nonblock(net_t *net)
{
    nonblock_calls++;
    if (inject_nonblock_failure) return -1;
    int flags = fcntl(net->fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(net->fd, F_SETFL, flags | O_NONBLOCK);
}
static int mbedtls_ssl_close_notify(ssl_t *ssl)
{
    notify_calls++;
    return (int)write(ssl->net->fd, "x", 1);
}
static void mbedtls_net_free(net_t *net)
{
    if (net->fd >= 0) { shutdown(net->fd, SHUT_RDWR); close(net->fd); net->fd = -1; }
    releases++;
}
static void mbedtls_ssl_free(ssl_t *ssl) { (void)ssl; releases++; }
static void mbedtls_ssl_config_free(int *cfg) { (void)cfg; releases++; }
static void mbedtls_ctr_drbg_free(int *drbg) { (void)drbg; releases++; }

ACTUAL_CLEANUP

int main(int argc, char **argv)
{
    assert(argc == 2);
    tls_ctx_t ctx = {0};
    ctx.net.fd = -1; ctx.ssl.net = &ctx.net;
    int peer = -1;
    if (strcmp(argv[1], "invalid")) {
        int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        ctx.net.fd = pair[0]; peer = pair[1];
        if (!strcmp(argv[1], "full")) {
            int size = 1024;
            assert(setsockopt(ctx.net.fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
            int flags = fcntl(ctx.net.fd, F_GETFL, 0);
            assert(fcntl(ctx.net.fd, F_SETFL, flags | O_NONBLOCK) == 0);
            char bytes[4096] = {0};
            while (write(ctx.net.fd, bytes, sizeof(bytes)) > 0) {}
            while (write(ctx.net.fd, bytes, 1) > 0) {}
            assert(errno == EAGAIN || errno == EWOULDBLOCK);
            assert(fcntl(ctx.net.fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
        }
    }
    inject_nonblock_failure = !strcmp(argv[1], "set-fails");
    tls_ctx_free(&ctx);
    assert(releases == 4 && ctx.net.fd == -1);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    if (!strcmp(argv[1], "invalid")) assert(nonblock_calls == 0 && notify_calls == 0);
    else if (inject_nonblock_failure) assert(nonblock_calls == 1 && notify_calls == 0);
    else assert(nonblock_calls == 1 && notify_calls == 1);
#else
    assert(nonblock_calls == 0 && notify_calls == 1);
#endif
    (void)mbedtls_net_set_nonblock;
    if (peer >= 0) close(peer);
    puts("TLS cleanup returned and released every context component");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root', type=Path, required=True)
    parser.add_argument('--expect-broken', action='store_true')
    args = parser.parse_args()
    text = (args.agent_root / 'src/infra/vela_tls.c').read_text()
    start = text.index('static void tls_ctx_free(tls_ctx_t* ctx)')
    end = text.index('\n}\n', start) + 3
    function = text[start:end]
    assert function.count('mbedtls_net_set_nonblock') == 1
    if args.expect_broken:
        function, count = re.subn(r'#ifdef CONFIG_EXAMPLES_ZHAOXI\n.*?#else\n(.*?)#endif\n',
                                 r'\1', function, flags=re.S)
        assert count == 1
    with tempfile.TemporaryDirectory(prefix='zhaoxi-tls-cleanup-') as temporary:
        root = Path(temporary); source = root / 'test.c'
        source.write_text(HARNESS.replace('ACTUAL_CLEANUP', function))
        for product in ([True] if args.expect_broken else [True, False]):
            target = root / ('product' if product else 'standard')
            command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                       '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if product:
                command += ['-DCONFIG_EXAMPLES_ZHAOXI=1']
            subprocess.run(command + [str(source), '-o', str(target)], check=True)
            cases = ['full'] if args.expect_broken else (
                ['full', 'invalid', 'set-fails', 'writable'] if product else ['writable'])
            for case in cases:
                try:
                    result = subprocess.run([str(target), case], capture_output=True,
                                            text=True, timeout=2)
                except subprocess.TimeoutExpired:
                    if not args.expect_broken:
                        raise
                    print('before fix: cleanup blocks on a full socket with an unresponsive peer')
                    continue
                if args.expect_broken:
                    raise RuntimeError('Expected blocked cleanup did not reproduce:\n'
                                       + result.stdout + result.stderr)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print(('product' if product else 'standard') + ': ' + case + ' passed')


if __name__ == '__main__':
    main()

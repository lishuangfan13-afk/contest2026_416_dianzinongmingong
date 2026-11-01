/* SPDX-License-Identifier: Apache-2.0 */
#include "mimo_sse.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char output[128];
static size_t used;
static int cancelled;
static int sink(const unsigned char *pcm, size_t length, void *user)
{
    assert(user == output && length % 2 == 0);
    if (cancelled) return -ECANCELED;
    assert(used + length <= sizeof(output));
    memcpy(output + used, pcm, length); used += length; return 0;
}
static mimo_sse_t *start(void)
{
    used = 0; cancelled = 0;
    mimo_sse_t *s = mimo_sse_create(sink, output); assert(s); return s;
}
static const char audio[] = "data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"AAABAAIAAwA=\"}},\"finish_reason\":null}]}\r\n\r\n";
static const char ending[] = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
static int feed(mimo_sse_t *s, const char *bytes) { return mimo_sse_feed(s, (const unsigned char *)bytes, strlen(bytes)); }
int main(void)
{
    char fixture[512]; snprintf(fixture, sizeof(fixture), ": keepalive\nevent: message\nid: 1\n%s%s", audio, ending);
    size_t n = strlen(fixture);
    for (size_t split = 0; split <= n; split++) {
        mimo_sse_t *s = start();
        assert(mimo_sse_feed(s, (const unsigned char *)fixture, split) == 0);
        assert(mimo_sse_feed(s, (const unsigned char *)fixture + split, n - split) == 0);
        assert(mimo_sse_finish(s) == 0 && used == 8 && mimo_sse_pcm_bytes(s) == 8);
        static const unsigned char expected[] = {0,0,1,0,2,0,3,0};
        assert(!memcmp(output, expected, 8)); mimo_sse_destroy(s);
    }
    mimo_sse_t *s = start();
    for (size_t i = 0; i < n; i++) assert(!mimo_sse_feed(s, (const unsigned char *)fixture + i, 1));
    assert(!mimo_sse_finish(s)); mimo_sse_destroy(s);
    s = start();
    assert(!feed(s,"data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"AQ==\"}}}]}\n\n"));
    assert(used == 0);
    assert(!feed(s,"data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"AgME\"}}}]}\n\n"));
    assert(!feed(s, ending) && !mimo_sse_finish(s));
    assert(used == 4 && output[0] == 1 && output[1] == 2 && output[2] == 3 && output[3] == 4);
    mimo_sse_destroy(s);
    const char *bad[] = {
        "data: {invalid}\n\n", "data: {\"error\":{\"message\":\"failed\"}}\n\n",
        "data: {\"choices\":{}}\n\n", "data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":3}}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"!!bad!!\"}}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n",
        "data: {\"choices\":[]}garbage\n\n"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        s = start(); assert(feed(s, bad[i]) < 0); assert(mimo_sse_finish(s) < 0); mimo_sse_destroy(s);
    }
    s = start(); assert(!feed(s, audio)); assert(mimo_sse_finish(s) == -EPROTO); mimo_sse_destroy(s);
    s = start(); assert(!feed(s, ending)); assert(mimo_sse_finish(s) == -ENODATA); mimo_sse_destroy(s);
    s = start(); cancelled = 1; assert(feed(s, audio) == -ECANCELED); assert(feed(s, ending) == -ECANCELED); mimo_sse_destroy(s);
    s = start(); assert(!feed(s,"data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"AQ==\"}}}]}\n\n"));
    assert(!feed(s,ending)); assert(mimo_sse_finish(s) == -EPROTO); mimo_sse_destroy(s);
    s = start();
    char *large = malloc(256*1024+2); assert(large); memset(large,'x',256*1024+1);
    assert(mimo_sse_feed(s,(unsigned char *)large,256*1024+1) == -EOVERFLOW); free(large); mimo_sse_destroy(s);
    s = start(); unsigned char nul[] = {'d','a','t','a',':',0};
    assert(mimo_sse_feed(s,nul,sizeof(nul)) == -EPROTO); mimo_sse_destroy(s);
    puts("PASS: actual SSE/base64 parser split boundaries, samples, terminal events, failures, cancellation and bounded buffers.");
    return 0;
}

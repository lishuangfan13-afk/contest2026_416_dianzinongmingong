/* SPDX-License-Identifier: Apache-2.0
 * Actual provider + SSE parser; deterministic delayed/fragmented HTTPS only.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static size_t allocations, live, fail_at, reallocations;
static size_t transport_failure_offset, transport_allocations, setup_allocations;
static void *fixture_malloc(size_t bytes)
{
    if (++allocations == fail_at) return NULL;
    void *p = malloc(bytes); if (p) live++; return p;
}
static void *fixture_calloc(size_t count, size_t bytes)
{
    if (++allocations == fail_at) return NULL;
    void *p = calloc(count, bytes); if (p) live++; return p;
}
static void *fixture_realloc(void *old, size_t bytes)
{
    assert(bytes); reallocations++;
    if (++allocations == fail_at) return NULL;
    bool had_old = old != NULL;
    void *p = realloc(old, bytes); if (p && !had_old) live++; return p;
}
static void fixture_free(void *p)
{
    /* cJSON_PrintUnformatted allocates outside this translation unit. Its
     * payload is freed by the provider too, so track owned allocations by
     * hooks installed in main, covering cJSON and parser allocations alike. */
    if (p) { assert(live); live--; free(p); }
}
static int64_t now_ms;
static int fixture_clock(clockid_t id, struct timespec *out)
{ (void)id; out->tv_sec=now_ms/1000; out->tv_nsec=now_ms%1000*1000000; return 0; }
#define malloc fixture_malloc
#define calloc fixture_calloc
#define realloc fixture_realloc
#define free fixture_free
#define clock_gettime fixture_clock
#include MIMO_SSE_SOURCE
#include MIMO_SOURCE
#undef clock_gettime
#undef malloc
#undef calloc
#undef realloc
#undef free

enum scenario { VALID, MISSING_END, BAD_TRAILER, BAD_BASE64, ODD_PCM,
                HTTP_FAIL, HTTP_REDIRECT, NETWORK_FAIL, CANCEL_MID,
                STOP_BEFORE_DRAIN, ERROR_BEFORE_DRAIN, OVERSIZE,
                EMPTY_AUDIO, FINISH_ONLY, HTTP_EMPTY, EXACT_LIMIT };
static enum scenario scenario;
static bool pc, returned, in_transport, stopped;
static int stream_error, cancel_on_callback, stop_on_callback, error_on_end;
static size_t delivered, emitted, calls, ends, largest, wire_events;
static unsigned budget_seen;
static unsigned char sample(size_t i) { return (unsigned char)((i*37u+i/251u+19u)&255u); }
bool agent_shutdown_requested(void) { return stopped; }
int voice_channel_tts_stream_error(void) { return stream_error; }
int claw_config_get(const char *key, char *out, size_t capacity)
{
    const char *value = !strcmp(key,"api_key") ? "fixture-not-a-real-key" :
        !strcmp(key,"mimo_speech_host") ? "api.xiaomimimo.com" :
        !strcmp(key,"uplink_mode") ? (pc?"pc":"wifi") : NULL;
    if (!value) return -1;
    snprintf(out,capacity,"%s",value); return 0;
}
const char *zhaoxi_prompt_id(const char *text) { (void)text; return NULL; }
int zhaoxi_prompt_stream(const char *id,zhaoxi_prompt_emit emit,
    zhaoxi_prompt_cancel cancel,void *user,bool *handled)
{ (void)id;(void)emit;(void)cancel;(void)user;(void)handled;assert(0);return -1; }

static void output(const unsigned char *pcm,size_t length,int final,void *user)
{
    assert(user == &emitted);
    if (pc) {
        /* A parser callback during transport, even after the last PCM event,
         * is forbidden. Missing/trailing-invalid finish is tested separately. */
        assert(returned && !in_transport);
        assert(scenario==VALID || scenario==FINISH_ONLY || scenario==STOP_BEFORE_DRAIN ||
               scenario==ERROR_BEFORE_DRAIN || scenario==EXACT_LIMIT);
    } else if (!final) assert(in_transport && !returned);
    if (final) {
        assert(!pcm && !length && !ends); ends++;
        if (error_on_end) stream_error=-EPIPE;
        return;
    }
    assert(pcm && length && !(length%2) && !ends);
    assert(length<=3200);
    for(size_t i=0;i<length;i++) assert(pcm[i]==sample(emitted+i));
    emitted+=length; calls++; if(length>largest)largest=length;
    if(cancel_on_callback && calls==(size_t)cancel_on_callback)stream_error=-ECANCELED;
    if(stop_on_callback && calls==(size_t)stop_on_callback)stopped=true;
}

int vela_https_post_json_bounded(const char *host,const char *port,const char *path,
    const vela_header_t *headers,const char *body,char *response,size_t capacity,
    unsigned budget,int (*cancel)(void *),void *user)
{
    (void)host;(void)port;(void)path;(void)headers;(void)body;(void)response;
    (void)capacity;(void)budget;(void)cancel;(void)user;
    assert(!"Batch transport must never serve streaming regression");return -EIO;
}

static int fragmented(vela_https_body_cb sink,void *arg,const char *wire,size_t bytes)
{
    static const size_t pieces[]={1,7,503,31,4093};
    size_t offset=0,part=0;
    while(offset<bytes) {
        size_t n=pieces[part++%5]; if(n>bytes-offset)n=bytes-offset;
        int rc=sink((const unsigned char *)wire+offset,n,arg);
        if(rc)return rc;
        offset+=n;
        if(pc)assert(!calls && !ends && !emitted);
    }
    return 0;
}
static int pcm_event(vela_https_body_cb sink,void *arg,size_t bytes)
{
    /* Fixture construction uses libc allocation, outside failure injection. */
    unsigned char *raw=malloc(bytes),*b64=malloc(4*((bytes+2)/3)+1);
    assert(raw && b64);
    for(size_t i=0;i<bytes;i++)raw[i]=sample(delivered+i);
    size_t encoded=0;
    assert(!mbedtls_base64_encode(b64,4*((bytes+2)/3)+1,&encoded,raw,bytes));
    const char *prefix="data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"";
    const char *suffix="\"}}}]}\r\n\r\n";
    size_t length=strlen(prefix)+encoded+strlen(suffix);
    char *wire=malloc(length+1);assert(wire);
    snprintf(wire,length+1,"%s%s%s",prefix,b64,suffix);
    now_ms+=750; /* Deliberate supply gaps, without sleeps or real network. */
    int rc=fragmented(sink,arg,wire,length);
    free(wire);free(raw);free(b64);
    if(!rc){delivered+=bytes;wire_events++;}
    return rc;
}
int vela_https_post_json_stream_bounded(const char *host,const char *port,const char *path,
    const vela_header_t *headers,const char *body,vela_https_body_cb sink,void *arg,
    unsigned budget,int (*cancel)(void *),void *cancel_arg)
{
    assert(!strcmp(host,"api.xiaomimimo.com") && !strcmp(port,"443"));
    assert(!strcmp(path,"/v1/chat/completions") && headers && body);
    assert(budget==180000);budget_seen=budget;
    /* Request parsed with actual cJSON; production failures before here must
     * not enter the transport with a silently incomplete request. */
    setup_allocations=allocations;
    cJSON *request=cJSON_Parse(body);assert(request);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(request,"stream")));cJSON_Delete(request);
    in_transport=true;
    transport_allocations=allocations;
    if(transport_failure_offset)fail_at=allocations+transport_failure_offset;
    int rc=0;
    if(cancel(cancel_arg)){rc=-ECANCELED;goto done;}
    if(scenario==HTTP_EMPTY){rc=503;goto done;}
    if(scenario!=EMPTY_AUDIO) {
        const size_t sizes[]={1,3199,48001,50003};
        size_t count=scenario==OVERSIZE?61:scenario==EXACT_LIMIT?60:4;
        for(size_t i=0;i<count;i++) {
            size_t bytes=(scenario==OVERSIZE || scenario==EXACT_LIMIT)?(i==60?2:96000):sizes[i];
            if(scenario==ODD_PCM && i==3)bytes--;
            rc=pcm_event(sink,arg,bytes);if(rc)goto done;
            if(scenario==CANCEL_MID && i==1)stopped=true;
            if(cancel(cancel_arg)){rc=-ECANCELED;goto done;}
        }
    }
    if(scenario==BAD_BASE64) {
        const char *bad="data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"!!!!\"}}}]}\n\n";
        rc=fragmented(sink,arg,bad,strlen(bad));
    }
    if(rc)goto done;
    if(scenario!=MISSING_END) {
        const char *end=scenario==FINISH_ONLY?
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n":"data: [DONE]\n\n";
        rc=fragmented(sink,arg,end,strlen(end));if(rc)goto done;
    }
    if(scenario==BAD_TRAILER){const char *bad="data: {\"choices\":[]}\n\n";rc=fragmented(sink,arg,bad,strlen(bad));if(rc)goto done;}
    if(scenario==STOP_BEFORE_DRAIN)stopped=true;
    if(scenario==ERROR_BEFORE_DRAIN)stream_error=-EPIPE;
    rc=scenario==HTTP_FAIL?503:scenario==HTTP_REDIRECT?302:scenario==NETWORK_FAIL?-ETIMEDOUT:200;
done:
    if(pc)assert(!emitted && !calls && !ends);
    in_transport=false;returned=true;return rc;
}
static void reset(enum scenario next,bool buffered)
{
    assert(!live);scenario=next;pc=buffered;returned=in_transport=stopped=false;
    stream_error=cancel_on_callback=stop_on_callback=error_on_end=0;
    delivered=emitted=calls=ends=largest=wire_events=0;
    allocations=reallocations=fail_at=0;transport_failure_offset=transport_allocations=0;now_ms=0;budget_seen=0;
}
static int speak(void)
{
    int rc=zhaoxi_mimo_tts_stream("fixture speech",output,&emitted);
    assert(!live);return rc;
}
int main(void)
{
    /* cJSON owns request/payload/parsed events; including it in accounting
     * proves every scenario frees both growing prefetch and parser/JSON data. */
    cJSON_Hooks hooks={fixture_malloc,fixture_free};cJSON_InitHooks(&hooks);
    reset(VALID,true);assert(!speak());
    assert(emitted==101204 && emitted==delivered && ends==1 && calls==32 && largest==3200);
    assert(now_ms==3000 && wire_events==4 && reallocations>=3 && budget_seen==180000);
    reset(VALID,false);assert(!speak());assert(emitted==101204 && ends==1 && !reallocations);
    reset(FINISH_ONLY,true);assert(!speak());assert(emitted==101204 && ends==1);
    reset(EXACT_LIMIT,true);assert(!speak());assert(emitted==5760000 && ends==1);
    const struct {enum scenario what;int expected;} failures[]={
        {MISSING_END,-EPROTO},{BAD_TRAILER,-EPROTO},{BAD_BASE64,-EPROTO},
        {ODD_PCM,-EPROTO},{HTTP_FAIL,-EIO},{HTTP_REDIRECT,-EIO},
        {NETWORK_FAIL,-ETIMEDOUT},{CANCEL_MID,-ECANCELED},
        {STOP_BEFORE_DRAIN,-ECANCELED},{ERROR_BEFORE_DRAIN,-EPIPE},
        {OVERSIZE,-EFBIG},{EMPTY_AUDIO,-ENODATA},{HTTP_EMPTY,-EIO}};
    for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
        reset(failures[i].what,true);assert(speak()==failures[i].expected);
        assert(!emitted && !calls && !ends);
    }
    reset(VALID,true);cancel_on_callback=2;assert(speak()==-ECANCELED);
    assert(emitted==6400 && calls==2 && !ends);
    reset(VALID,true);stop_on_callback=1;assert(speak()==-ECANCELED);
    assert(emitted==3200 && calls==1 && !ends);
    reset(VALID,true);error_on_end=1;assert(speak()==-EPIPE);assert(ends==1 && emitted==101204);
    reset(VALID,false);cancel_on_callback=2;assert(speak()==-ECANCELED);assert(calls==2 && !ends);
    /* Inject each production allocator operation after the request has been
     * constructed. Failing cJSON's unchecked request builders is a different
     * existing contract; prefetch/parser failure isolation is the regression. */
    reset(VALID,true);assert(!speak());size_t total=allocations,start=transport_allocations,setup=setup_allocations;
    assert(total>start);
    for(size_t nth=1;nth<=total-start;nth++) {
        reset(VALID,true);transport_failure_offset=nth;
        int rc=speak();assert(rc==-ENOMEM || rc==-EPROTO);
        assert(allocations>=fail_at && !emitted && !calls && !ends);
    }
    reset(VALID,true);fail_at=1;assert(speak()==-ENOMEM);assert(!emitted && !ends);
    /* Three allocations immediately before the mock transport are the actual
     * SSE object plus its line/event buffers. Fail each independently. */
    for(size_t nth=setup-2;nth<=setup;nth++) {
        reset(VALID,true);fail_at=nth;assert(speak()==-ENOMEM);
        assert(!returned && !emitted && !ends);
    }
    printf("PASS: %zu transport/parser/prefetch allocation-failure positions, all zero live allocations\n",total-start);
    puts("PASS: actual MiMo provider and SSE parser; delayed fragmented PC prefetch commit vs Wi-Fi streaming; exact PCM order; HTTP/protocol/cancel/limit and allocation cleanup");
    cJSON_InitHooks(NULL);return 0;
}

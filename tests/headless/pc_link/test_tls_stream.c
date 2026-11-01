#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "infra/vela_tls.h"

#define MBEDTLS_ERR_SSL_TIMEOUT -101
#define MBEDTLS_ERR_NET_RECV_FAILED -102
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY -103
#define MBEDTLS_ERR_SSL_WANT_READ -104
#define MBEDTLS_ERR_SSL_WANT_WRITE -105
typedef struct {int fd;} mbedtls_net_context;
typedef struct {void *bio;int (*recv)(void *,unsigned char *,size_t);} fake_ssl;
typedef struct {fake_ssl ssl;mbedtls_net_context net;} tls_ctx_t;
struct feed {const unsigned char *bytes;size_t length,position,fragment;
             bool want,early;int error_at,callback_error,connect_error,write_error;
             size_t calls,used;int64_t now,tick;int allocations,cleanups;
             bool bio_mode;unsigned net_failures;int net_errno,net_result;
             size_t net_calls,net_failure_at;int64_t net_tick;
             unsigned char output[70000];};
static _Thread_local struct feed feed;
static int test_clock_gettime(clockid_t id,struct timespec *out)
{
    (void)id;out->tv_sec=feed.now/1000;out->tv_nsec=feed.now%1000*1000000;return 0;
}
#define clock_gettime test_clock_gettime
static int test_setsockopt(int fd,int level,int option,const void *value,socklen_t length)
{
    assert(fd==42);assert(level==SOL_SOCKET);assert(option==SO_RCVTIMEO);
    assert(length==sizeof(struct timeval));
    const struct timeval *time=value;assert(time->tv_sec<=1);return 0;
}
#define setsockopt test_setsockopt
static int mbedtls_net_send(void *context,const unsigned char *bytes,size_t length)
{
    (void)bytes;assert(((mbedtls_net_context *)context)->fd==42);return (int)length;
}
static int mbedtls_net_recv(void *context,unsigned char *bytes,size_t length)
{
    assert(((mbedtls_net_context *)context)->fd==42);
    feed.net_calls++;feed.now+=feed.net_tick;
    if(feed.net_failures && feed.position>=feed.net_failure_at) {
        feed.net_failures--;errno=feed.net_errno;return feed.net_result;
    }
    if(!feed.bio_mode)return 1;
    size_t count=feed.length-feed.position;
    if(count>feed.fragment)count=feed.fragment;
    if(count>length)count=length;
    memcpy(bytes,feed.bytes+feed.position,count);feed.position+=count;
    return (int)count;
}
#ifdef CONFIG_ZHAOXI_BES_AUDIO
static int zhaoxi_tls_recv(void *context,unsigned char *bytes,size_t length)
{
    return mbedtls_net_recv(context,bytes,length);
}
#endif
static int mbedtls_ssl_read(fake_ssl *ssl,unsigned char *out,size_t capacity)
{
    if(feed.bio_mode) {assert(ssl->recv);return ssl->recv(ssl->bio,out,capacity);}
    feed.now+=feed.tick;
    if(feed.want){feed.want=false;return MBEDTLS_ERR_SSL_WANT_READ;}
    if(feed.error_at>=0 && feed.position>=(size_t)feed.error_at)return -999;
    if(feed.position==feed.length)return 0;
    size_t count=feed.length-feed.position;
    if(count>feed.fragment)count=feed.fragment;
    if(count>capacity)count=capacity;
    memcpy(out,feed.bytes+feed.position,count);feed.position+=count;return (int)count;
}
static void mbedtls_ssl_set_bio(fake_ssl *ssl,void *context,
    int (*send)(void *,const unsigned char *,size_t),
    int (*recv)(void *,unsigned char *,size_t),void *timeout)
{
    assert(send && recv && !timeout);ssl->bio=context;ssl->recv=recv;
}
static int tls_ctx_connect(tls_ctx_t *ctx,const char *host,const char *port)
{
    assert(host && port);feed.allocations++;ctx->net.fd=42;ctx->ssl.bio=&ctx->net;
    return feed.connect_error;
}
static int tls_write_request(tls_ctx_t *ctx,const char *method,const char *host,
    const char *path,const vela_header_t *headers,const char *body,size_t length)
{
    assert(ctx->net.fd==42 && !strcmp(method,"POST") && host && path && body && length);
    bool accept=false,encoding=false,auth=false;
    for(;headers->name;headers++){
        if(!strcmp(headers->name,"Accept"))accept=!strcmp(headers->value,"text/event-stream");
        if(!strcmp(headers->name,"Accept-Encoding"))encoding=!strcmp(headers->value,"identity");
        if(!strcmp(headers->name,"api-key"))auth=true;
    }
    assert(accept && encoding && auth);return feed.write_error;
}
static void tls_ctx_free(tls_ctx_t *ctx)
{
    assert(ctx->ssl.bio==&ctx->net);feed.cleanups++;
}

/* ACTUAL_STREAM_IMPLEMENTATION */

static int receive(const unsigned char *bytes,size_t count,void *user)
{
    assert(user==&feed);feed.calls++;
    if(feed.position<feed.length)feed.early=true;
    if(feed.callback_error)return feed.callback_error;
    assert(feed.used+count<=sizeof(feed.output));
    memcpy(feed.output+feed.used,bytes,count);feed.used+=count;return 0;
}
static void reset(const unsigned char *wire,size_t length,size_t fragment)
{
    memset(&feed,0,sizeof(feed));feed.bytes=wire;feed.length=length;
    feed.fragment=fragment;feed.error_at=-1;feed.now=1000;
}
static int parse(const char *wire,size_t fragment)
{
    reset((const unsigned char *)wire,strlen(wire),fragment);
    tls_ctx_t ctx={0};
    return tls_read_response_stream(&ctx,receive,&feed,10000);
}
static void expect(const char *wire,int status,const char *body)
{
    for(size_t f=1;f<=64;f++){
        assert(parse(wire,f)==status);
        if(body){assert(feed.used==strlen(body));assert(!memcmp(feed.output,body,feed.used));}
        else if(status>0 && status!=200)assert(!feed.calls);
    }
}
static int request(const char *wire)
{
    reset((const unsigned char *)wire,strlen(wire),7);
    vela_header_t headers[]={{"api-key","TEST_ONLY"},{NULL,NULL}};
    return vela_https_post_json_stream("example.invalid","443","/v1/test",headers,"{}",receive,&feed);
}
static void *parallel(void *unused)
{
    (void)unused;
    for(int i=0;i<20;i++){
        assert(request("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")==200);
        assert(feed.allocations==1 && feed.cleanups==1 && feed.used==2);
    }
    return NULL;
}
int main(void)
{
    expect("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",200,"hello");
    expect("HTTP/1.1 200 OK\r\ncontent-length:\t5 \r\nContent-Encoding: identity\r\n\r\nhello",200,"hello");
    expect("HTTP/1.0 200 OK\r\n\r\nclose-delimited",200,"close-delimited");
    expect("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n",200,"");
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;ext=test\r\nhe\r\n3\r\nllo\r\n0\r\nX-End: yes\r\n\r\n",200,"hello");
    expect("HTTP/1.1 429 Limit\r\nContent-Length: 10\r\n\r\nnot audio!",429,NULL);
    expect("HTTP/1.1 401 Unauthorized\r\n\r\n{\"error\":\"denied\"}",401,NULL);
    const char *bad[]={
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX: \001\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nh",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhiXX0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n+2\r\nhi\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\nhi\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nBroken trailer\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "NOTHTTP 200 OK\r\n\r\n",
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)expect(bad[i],VELA_TLS_ERR_PROTOCOL,NULL);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 33554433\r\n\r\n",VELA_TLS_ERR_OVERFLOW,NULL);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\n",VELA_TLS_ERR_OVERFLOW,NULL);
    char large[70000];
    memset(large,'x',sizeof(large));memcpy(large,"HTTP/1.1 200 OK\r\nX: ",20);large[sizeof(large)-1]=0;
    assert(parse(large,31)==VELA_TLS_ERR_OVERFLOW);
    const char *valid="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    tls_ctx_t ctx={0};
    reset((const unsigned char *)valid,strlen(valid),1);feed.callback_error=-ECANCELED;
    assert(tls_read_response_stream(&ctx,receive,&feed,10000)==-ECANCELED);
    reset((const unsigned char *)valid,strlen(valid),1);feed.callback_error=1;
    assert(tls_read_response_stream(&ctx,receive,&feed,10000)==VELA_TLS_ERR_PROTOCOL);
    reset((const unsigned char *)valid,strlen(valid),1);feed.want=true;
    assert(tls_read_response_stream(&ctx,receive,&feed,10000)==200);
    reset((const unsigned char *)valid,strlen(valid),1);feed.error_at=10;
    assert(tls_read_response_stream(&ctx,receive,&feed,10000)==VELA_TLS_ERR_READ);
    reset((const unsigned char *)valid,strlen(valid),1);feed.tick=1000;
    assert(tls_read_response_stream(&ctx,receive,&feed,2500)==VELA_TLS_ERR_TIMEOUT);
    reset((const unsigned char *)valid,strlen(valid),5);
    tls_stream_reader_t reader={.ctx=&ctx,.deadline=10000,.delivered=TLS_STREAM_MAX_BODY};
    assert(tls_stream_deliver(&reader,1,receive,&feed)==VELA_TLS_ERR_OVERFLOW);
    size_t prefix=(size_t)snprintf(large,sizeof(large),"HTTP/1.1 200 OK\r\nContent-Length: 60000\r\n\r\n");
    memset(large+prefix,'a',60000);large[prefix+60000]=0;
    assert(parse(large,19)==200 && feed.used==60000 && feed.early);
    assert(request(valid)==200 && feed.allocations==1 && feed.cleanups==1);
    reset((const unsigned char *)valid,strlen(valid),2);feed.connect_error=VELA_TLS_ERR_CONNECT;
    vela_header_t extra[]={{"api-key","TEST_ONLY"},{NULL,NULL}};
    assert(vela_https_post_json_stream("example.invalid","443","/v1/test",extra,"{}",receive,&feed)==VELA_TLS_ERR_CONNECT);
    assert(feed.cleanups==1);
    reset((const unsigned char *)valid,strlen(valid),2);feed.write_error=VELA_TLS_ERR_WRITE;
    assert(vela_https_post_json_stream("example.invalid","443","/v1/test",extra,"{}",receive,&feed)==VELA_TLS_ERR_WRITE && feed.cleanups==1);
    assert(vela_https_post_json_stream(NULL,"443","/test",extra,"{}",receive,&feed)==VELA_TLS_ERR_PROTOCOL);
    mbedtls_net_context net={.fd=42};tls_stream_bio_t bio={.net=&net,.deadline=feed.now+1000};unsigned char byte;
    assert(tls_stream_recv(&bio,&byte,1)==1);assert(tls_stream_send(&bio,&byte,1)==1);
    const int transient[]={EAGAIN,EWOULDBLOCK,EINTR,ETIMEDOUT};
    for(size_t i=0;i<sizeof(transient)/sizeof(*transient);i++) {
        feed.net_failures=1;feed.net_errno=transient[i];feed.net_result=MBEDTLS_ERR_NET_RECV_FAILED;
        assert(tls_stream_recv(&bio,&byte,1)==MBEDTLS_ERR_SSL_WANT_READ);
        assert(tls_stream_recv(&bio,&byte,1)==1);
    }
    feed.net_failures=1;feed.net_errno=EINTR;feed.net_result=MBEDTLS_ERR_SSL_WANT_READ;
    assert(tls_stream_recv(&bio,&byte,1)==MBEDTLS_ERR_SSL_WANT_READ);
    feed.net_failures=1;feed.net_errno=ECONNRESET;feed.net_result=MBEDTLS_ERR_NET_RECV_FAILED;
    assert(tls_stream_recv(&bio,&byte,1)==MBEDTLS_ERR_NET_RECV_FAILED);
    bio.deadline=feed.now;assert(tls_stream_recv(&bio,&byte,1)==MBEDTLS_ERR_SSL_TIMEOUT);
    assert(tls_stream_send(&bio,&byte,1)==MBEDTLS_ERR_SSL_TIMEOUT);
    /* A blocking BIO timeout slice after successful PCM delivery must recover.
     * Exercise the real public request, BIO and HTTP parser together. */
    reset((const unsigned char *)valid,strlen(valid),3);feed.bio_mode=true;
    feed.net_failures=3;feed.net_errno=EAGAIN;feed.net_result=MBEDTLS_ERR_NET_RECV_FAILED;
    feed.net_failure_at=strlen(valid)-2;
    feed.net_tick=1000;
    assert(vela_https_post_json_stream("example.invalid","443","/v1/test",extra,"{}",receive,&feed)==200);
    assert(feed.used==5 && !memcmp(feed.output,"hello",5) && feed.cleanups==1);
    /* Repeated timeout slices still hit the original absolute response deadline. */
    reset((const unsigned char *)valid,strlen(valid),3);feed.bio_mode=true;
    feed.net_failures=1000;feed.net_errno=EAGAIN;feed.net_result=MBEDTLS_ERR_NET_RECV_FAILED;
    feed.net_tick=1000;
    assert(vela_https_post_json_stream("example.invalid","443","/v1/test",extra,"{}",receive,&feed)==VELA_TLS_ERR_TIMEOUT);
    assert(feed.now==181000 && !feed.used && feed.cleanups==1 && feed.net_calls==180);
    /* Deadline reached inside the blocking read is not another WANT_READ. */
    reset((const unsigned char *)valid,strlen(valid),3);
    bio.deadline=feed.now+1000;feed.net_tick=1000;
    feed.net_failures=1;feed.net_errno=EAGAIN;feed.net_result=MBEDTLS_ERR_NET_RECV_FAILED;
    assert(tls_stream_recv(&bio,&byte,1)==MBEDTLS_ERR_SSL_TIMEOUT);
    pthread_t one,two;assert(!pthread_create(&one,NULL,parallel,NULL));assert(!pthread_create(&two,NULL,parallel,NULL));
    pthread_join(one,NULL);pthread_join(two,NULL);
    puts("PASS: actual incremental TLS body parser, every 1..64-byte split, framing/errors/cancel/deadline/early-delivery, blocking BIO EAGAIN/EINTR recovery and per-caller cleanup.");
    return 0;
}

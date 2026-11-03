#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include "infra/vela_tls.h"

#define MBEDTLS_ERR_SSL_WANT_READ -101
#define MBEDTLS_ERR_SSL_WANT_WRITE -102
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY -103
#define MBEDTLS_ERR_NET_RECV_FAILED -104
#define TLS_RAW_BUF_SIZE 8192
#define TLS_STREAM_MAX_BODY (32u * 1024u * 1024u)
#define AGENT_LLM_SOCKET_TIMEOUT_SEC 35
#define MBEDTLS_ERR_SSL_TIMEOUT -105
typedef struct {int fd;} mbedtls_net_context;
typedef struct {int marker;} fake_ssl;
typedef struct {fake_ssl ssl;mbedtls_net_context net;int64_t request_deadline;size_t write_limit;} tls_ctx_t;
static const char *TAG="fixture";
static struct {
    unsigned char wire[100000];size_t used;
    int returns[16],errors[16];unsigned plan,index,calls;
    size_t record_limit;
    size_t largest_write;
    char logs[16000];size_t logged;
    int64_t now;
    const char *response;size_t response_at;
    int read_error;size_t fragment;
    int ioctl_error;
    const char *socket_path;
    unsigned raw_acquires,raw_releases,ioctls;
} f;

static void fixture_syslog(int priority,const char *format,...)
{
    (void)priority;va_list ap;va_start(ap,format);
    int n=vsnprintf(f.logs+f.logged,sizeof(f.logs)-f.logged,format,ap);
    va_end(ap);assert(n>=0 && (size_t)n<sizeof(f.logs)-f.logged);
    f.logged+=(size_t)n;errno=E2BIG; /* Diagnostics must restore this. */
}
#define syslog fixture_syslog

#ifdef CONFIG_ZHAOXI_BES_AUDIO
#define FIOC_FILEPATH 0xab01
#ifndef FIONSPACE
#define FIONSPACE 0xab02
#endif
static int fixture_ioctl(int fd,unsigned long request,...)
{
    assert(fd==42);va_list ap;va_start(ap,request);
    unsigned long arg=va_arg(ap,unsigned long);va_end(ap);f.ioctls++;
    if(f.ioctl_error) {errno=f.ioctl_error;return -1;}
    if(request==FIONREAD)*(int *)(uintptr_t)arg=0;
    else if(request==FIONSPACE)*(int *)(uintptr_t)arg=8000;
    else if(request==FIOC_FILEPATH) {
        snprintf((char *)(uintptr_t)arg,PATH_MAX,"%s",f.socket_path);
    } else assert(!"Unexpected ioctl; SO_ERROR/TCP_INFO must not be queried");
    errno=ENOTTY;return 0;
}
#define ioctl fixture_ioctl
static int fixture_clock(clockid_t clock,struct timespec *out)
{
    assert(clock==CLOCK_MONOTONIC);f.now+=5;
    out->tv_sec=f.now/1000;out->tv_nsec=f.now%1000*1000000;return 0;
}
#define clock_gettime fixture_clock
#endif

static int mbedtls_ssl_write(fake_ssl *ssl,const unsigned char *bytes,size_t length)
{
    (void)ssl;int ret;f.calls++;
    if(length>f.largest_write)f.largest_write=length;
    if(f.index<f.plan) {
        errno=f.errors[f.index];ret=f.returns[f.index++];
        if(ret<=0)return ret;
    } else {ret=(int)length;errno=0;}
    if((size_t)ret>length)ret=(int)length;
    if((size_t)ret>f.record_limit)ret=(int)f.record_limit;
    assert(f.used+(size_t)ret<sizeof(f.wire));
    memcpy(f.wire+f.used,bytes,(size_t)ret);f.used+=(size_t)ret;
    return ret;
}

static int mbedtls_ssl_read(fake_ssl *ssl,unsigned char *bytes,size_t length)
{
    (void)ssl;
    if(f.read_error && f.response_at>=strlen(f.response)) {
        errno=ENOTCONN;return f.read_error;
    }
    size_t n=strlen(f.response)-f.response_at;
    if(n>length)n=length;
    if(n>f.fragment)n=f.fragment;
    memcpy(bytes,f.response+f.response_at,n);f.response_at+=n;return (int)n;
}
static char raw[TLS_RAW_BUF_SIZE];
static char *tls_raw_acquire(void) {f.raw_acquires++;return raw;}
static void tls_raw_release(char *p) {assert(p==raw);f.raw_releases++;}

/* ACTUAL_TLS_WRITER_READER */

static void reset(void)
{
    memset(&f,0,sizeof(f));f.record_limit=16384;f.now=1000;
    f.response="";f.fragment=SIZE_MAX;errno=0;
    f.socket_path="tcp:[192.0.2.1:50123<->203.0.113.8:443, tx 42/8192, rx 0/8192 + ofo 0, st 01, flg 6]";
}
static int post(tls_ctx_t *ctx,const char *body)
{
    vela_header_t headers[]={{"api-key","PRIVATE_KEY_SENTINEL"},
                            {"Content-Type","application/json"},{NULL,NULL}};
    return tls_write_request(ctx,"POST","example.invalid","/v1/test",headers,body,strlen(body));
}
static void assert_private(void)
{
    assert(!strstr(f.logs,"PRIVATE_KEY_SENTINEL"));
    assert(!strstr(f.logs,"SECRET_BODY_SENTINEL"));
    assert(!strstr(f.logs,"192.0.2.1"));
    assert(!strstr(f.logs,"203.0.113.8"));
    assert(!strstr(f.logs,"50123"));
}
int main(void)
{
    (void)tls_read_response_stream;
    (void)fixture_syslog; (void)TAG;
    tls_ctx_t ctx={.net={42}};
    char body[40001];memset(body,'x',40000);body[40000]=0;
    memcpy(body,"SECRET_BODY_SENTINEL",20);
    reset();f.returns[0]=7;f.returns[1]=17;
    f.returns[2]=MBEDTLS_ERR_SSL_WANT_WRITE;f.errors[2]=EAGAIN;
    f.plan=3;
    assert(post(&ctx,body)==0);
    assert(f.used>40000 && f.calls>=7);
    assert(strstr((const char *)f.wire,"Content-Length: 40000\r\n"));
    char *split=strstr((char *)f.wire,"\r\n\r\n");assert(split);
    assert(!memcmp(split+4,body,40000));
    assert((size_t)(split+4-(char *)f.wire)+40000==f.used);
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    assert(strstr(f.logs,"body=40000/40000"));
    assert(strstr(f.logs,"want_write=1"));
    assert(strstr(f.logs,"stage=post-tx"));
    assert(strstr(f.logs,"protocol=tcp"));
    assert(strstr(f.logs,"tx 42/8192, rx 0/8192 + ofo 0, st 01, flg 6"));
    assert(f.ioctls==3 && errno==0);
#else
    assert(!f.logged && !f.ioctls);
#endif
    assert_private();

#ifdef CONFIG_ZHAOXI_BES_AUDIO
    reset();f.ioctl_error=ENOTSOCK;errno=ENOTCONN;
    tls_diag_socket(&ctx,"unsupported-fd",0,MBEDTLS_ERR_NET_RECV_FAILED,ENOTCONN);
    assert(errno==ENOTCONN && f.ioctls==3);
    assert(strstr(f.logs,"protocol=unknown meta=unsupported"));
    reset();f.socket_path="udp:[192.0.2.1:50123<->203.0.113.8:443, flags=1]";
    tls_diag_socket(&ctx,"other-socket",0,0,0);
    assert(strstr(f.logs,"protocol=udp meta= flags=1"));assert_private();
    reset();f.socket_path="file:[PRIVATE_KEY_SENTINEL,SECRET_BODY_SENTINEL]";
    tls_diag_socket(&ctx,"replaced-fd",0,0,0);
    assert(strstr(f.logs,"protocol=other meta=unavailable"));assert_private();
#endif

    reset();f.returns[0]=10000;f.returns[1]=16;f.returns[2]=-555;
    f.errors[2]=EPIPE;f.plan=3;
    assert(post(&ctx,body)==VELA_TLS_ERR_WRITE);
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    assert(strstr(f.logs,"body=16/40000"));
    assert(strstr(f.logs,"last_ssl_ret=-555 last_errno=32 rc=-3"));
    assert(strstr(f.logs,"stage=write-error"));assert(errno==EPIPE);
#endif
    assert_private();

    reset();f.returns[0]=0;f.plan=1;
    assert(post(&ctx,body)==VELA_TLS_ERR_WRITE && !f.used);
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    assert(strstr(f.logs,"header=0/"));assert(strstr(f.logs,"body=0/40000"));
#endif
    reset();f.returns[0]=MBEDTLS_ERR_SSL_WANT_READ;f.plan=1;
    assert(post(&ctx,body)==VELA_TLS_ERR_WRITE); /* Existing behavior retained. */

    reset();f.read_error=MBEDTLS_ERR_NET_RECV_FAILED;
    char response[128];assert(tls_read_response(&ctx,response,sizeof(response),NULL,NULL,false)==VELA_TLS_ERR_READ);
    assert(f.raw_acquires==1 && f.raw_releases==1);
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    assert(strstr(f.logs,"stage=header-read-fail"));
    assert(strstr(f.logs,"plain_rx=0 ssl_ret=-104 bio_errno=107"));
#endif
    assert_private();

    reset();f.response="HTTP/1.1 200";f.fragment=3;f.read_error=MBEDTLS_ERR_NET_RECV_FAILED;
    assert(tls_read_response(&ctx,response,sizeof(response),NULL,NULL,false)==VELA_TLS_ERR_READ);
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    assert(strstr(f.logs,"plain_rx=12 ssl_ret=-104 bio_errno=107"));
#endif
    reset();f.response="HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nokay";
    size_t count=0;bool keep=true;
    assert(tls_read_response(&ctx,response,sizeof(response),&count,&keep,false)==200);
    assert(count==4 && !memcmp(response,"okay",4) && !keep);
    assert(f.raw_acquires==1 && f.raw_releases==1);
    assert_private();
    reset();ctx.write_limit=1024;
    assert(post(&ctx,body)==0 && f.largest_write<=1024);
    char *payload=strstr((char *)f.wire,"\r\n\r\n");assert(payload);payload+=4;
    assert(f.used-(size_t)(payload-(char *)f.wire)==40000 && !memcmp(payload,body,40000));
    ctx.write_limit=0;
    reset();ctx.request_deadline=1;
    f.response="HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nokay";
    assert(tls_read_response(&ctx,response,sizeof(response),NULL,NULL,false)==VELA_TLS_ERR_TIMEOUT);
    assert(!response[0]);ctx.request_deadline=0;
    puts("PASS: actual TLS writer/reader short-write accounting, record-sized progress, retry/failure preservation, socket metadata and private-log boundaries");
    return 0;
}

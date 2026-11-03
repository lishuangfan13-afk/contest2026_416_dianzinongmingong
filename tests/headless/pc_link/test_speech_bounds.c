#include <assert.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include "infra/vela_tls.h"
#define TLS_STREAM_TIMEOUT_MS 180000
typedef struct {int fd;} mbedtls_net_context;
typedef struct {int unused;} ssl_t;
typedef struct {mbedtls_net_context net;ssl_t ssl;int64_t request_deadline;
    int (*request_cancel)(void *);void *request_user;bool abortive_close;} tls_ctx_t;
typedef struct {mbedtls_net_context *net;int64_t deadline;} tls_stream_bio_t;
static struct {int64_t now,end;unsigned dial_budget,dials,frees,writes,reads;
    int dial_error,read_error,cancel;bool last_abort,get;} f;
static int64_t bounded_now_ms(void){return f.now;}
static int64_t tls_stream_now_ms(void){return f.now;}
static int cancelled(void *arg){assert(arg==&f);return f.cancel;}
static int tls_ctx_connect_limited(tls_ctx_t *ctx,const char *host,const char *port,
    unsigned budget,int (*cancel)(void *),void *user)
{
    assert(!strcmp(host,"example.invalid") && !strcmp(port,"443"));
    f.dials++;f.dial_budget=budget;f.now+=200;
    ctx->request_cancel=cancel;ctx->request_user=user;ctx->request_deadline=f.now+budget;
    return f.dial_error?f.dial_error:(cancel && cancel(user))?VELA_TLS_ERR_TIMEOUT:0;
}
static int tls_ctx_connect(tls_ctx_t *ctx,const char *host,const char *port)
{int rc=tls_ctx_connect_limited(ctx,host,port,0,NULL,NULL);ctx->request_deadline=0;return rc;}
static int tls_write_request(tls_ctx_t *ctx,const char *method,const char *host,const char *path,
    const vela_header_t *headers,const char *body,size_t size)
{
    (void)host;assert(!strcmp(path,"/test"));
    if(f.get){assert(!strcmp(method,"GET") && !size && !body);assert(!strcmp(headers[0].name,"Accept"));}
    else {assert(!strcmp(method,"POST") && size==2 && !strcmp(body,"{}"));assert(!strcmp(headers[0].name,"Content-Type"));}
    if(ctx->request_deadline)assert(ctx->request_deadline==f.end);
    f.writes++;f.now+=300;return 0;
}
static int tls_read_response(tls_ctx_t *ctx,char *out,size_t cap,size_t *used,bool *keep,bool head)
{
    (void)used;(void)keep;assert(!head && cap>=4 && ctx->request_deadline==f.end);f.reads++;
    strcpy(out,"bad");return f.read_error?f.read_error:200;
}
static int tls_read_response_stream(tls_ctx_t *ctx,vela_https_body_cb cb,void *user,int64_t end)
{
    f.reads++;assert(end==(ctx->request_deadline?f.end:f.now+TLS_STREAM_TIMEOUT_MS));
    if(f.read_error)return f.read_error;
    assert(!cb((const unsigned char *)"ok",2,user));return 200;
}
static void tls_ctx_free(tls_ctx_t *ctx){f.last_abort=ctx->abortive_close;f.frees++;}
static int mbedtls_net_send(void *ctx,const unsigned char *data,size_t size)
{(void)ctx;(void)data;return (int)size;}
static int mbedtls_net_recv(void *ctx,unsigned char *data,size_t size)
{(void)ctx;(void)data;(void)size;return 0;}
static int tls_stream_send(void *ctx,const unsigned char *data,size_t size)
{return mbedtls_net_send(ctx,data,size);}
static int tls_stream_recv(void *ctx,unsigned char *data,size_t size)
{return mbedtls_net_recv(ctx,data,size);}
static void mbedtls_ssl_set_bio(ssl_t *ssl,void *ctx,
    int (*send)(void *,const unsigned char *,size_t),int (*recv)(void *,unsigned char *,size_t),void *timeout)
{(void)ssl;(void)ctx;(void)send;(void)recv;(void)timeout;}
/* ACTUAL_BOUNDED_APIS */

static const char *TAG="fixture";
static const char *mode,*buffer;
static bool wifi_up,ppp_up;
static unsigned changes;
static int actual;
static int claw_config_get(const char *key,char *out,size_t cap)
{
    const char *value=!strcmp(key,"uplink_mode")?mode:!strcmp(key,"wifi_tls_tx_buffer")?buffer:NULL;
    if(!value)return -1;
    snprintf(out,cap,"%s",value);return 0;
}
static struct ifaddrs interfaces[2];static struct sockaddr_in addresses[2];
static int fake_getifaddrs(struct ifaddrs **list)
{
    memset(interfaces,0,sizeof(interfaces));memset(addresses,0,sizeof(addresses));
    for(int i=0;i<2;i++) {
        addresses[i].sin_family=AF_INET;addresses[i].sin_addr.s_addr=1;
        interfaces[i].ifa_addr=(struct sockaddr *)&addresses[i];
        interfaces[i].ifa_name=i?"ppp0":"wlan0";
        interfaces[i].ifa_flags=(i?ppp_up:wifi_up)?IFF_UP|IFF_RUNNING:0;
    }
    interfaces[0].ifa_next=&interfaces[1];*list=interfaces;return 0;
}
static void fake_freeifaddrs(struct ifaddrs *list){assert(list==interfaces);}
static int fake_setsockopt(int fd,int level,int opt,const void *value,socklen_t size)
{assert(fd==42 && level==SOL_SOCKET && opt==SO_SNDBUF && size==sizeof(int));actual=*(const int *)value;changes++;return 0;}
static int fake_getsockopt(int fd,int level,int opt,void *value,socklen_t *size)
{assert(fd==42 && level==SOL_SOCKET && opt==SO_SNDBUF && *size==sizeof(int));*(int *)value=actual;return 0;}
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#define setsockopt fake_setsockopt
#define getsockopt fake_getsockopt
static void fake_syslog(int priority,const char *format,...){(void)priority;(void)format;}
#define syslog fake_syslog
/* ACTUAL_WIFI_POLICY */

static int deliver(const unsigned char *data,size_t size,void *arg)
{assert(size==2 && !memcmp(data,"ok",2));(*(unsigned *)arg)++;return 0;}
static void reset(unsigned budget)
{memset(&f,0,sizeof(f));f.now=1000;f.end=1000+budget;}
int main(void)
{
    char out[16];unsigned delivered=0;
    reset(15000);f.get=true;
    assert(vela_https_get_bounded("example.invalid","443","/test",out,sizeof(out),15000,cancelled,&f)==200);
    assert(f.dial_budget==15000 && f.dials==1 && f.frees==1 && f.writes==1 && f.reads==1);
    reset(2000);f.get=true;f.read_error=VELA_TLS_ERR_TIMEOUT;
    assert(vela_https_get_bounded("example.invalid","443","/test",out,sizeof(out),2000,cancelled,&f)==VELA_TLS_ERR_TIMEOUT);
    assert(!out[0] && f.last_abort && f.frees==1);
    reset(2000);f.get=true;f.cancel=1;
    assert(vela_https_get_bounded("example.invalid","443","/test",out,sizeof(out),2000,cancelled,&f)<0);
    assert(!f.writes && f.frees==1);
    assert(vela_https_get_bounded("example.invalid","443","/test",out,sizeof(out),0,cancelled,&f)<0);
    reset(60000);assert(vela_https_post_json_bounded("example.invalid","443","/test",NULL,"{}",out,sizeof(out),60000,cancelled,&f)==200);
    assert(f.dial_budget==15000 && f.dials==1 && f.frees==1 && f.writes==1 && f.reads==1);
    reset(2000);f.read_error=VELA_TLS_ERR_TIMEOUT;
    assert(vela_https_post_json_bounded("example.invalid","443","/test",NULL,"{}",out,sizeof(out),2000,cancelled,&f)==VELA_TLS_ERR_TIMEOUT);
    assert(!out[0] && f.dial_budget==2000 && f.frees==1 && f.last_abort);
    reset(60000);f.cancel=1;
    assert(vela_https_post_json_bounded("example.invalid","443","/test",NULL,"{}",out,sizeof(out),60000,cancelled,&f)==VELA_TLS_ERR_TIMEOUT);
    assert(!f.writes && !f.reads && f.frees==1 && f.last_abort);
    reset(180000);
    assert(vela_https_post_json_stream_bounded("example.invalid","443","/test",NULL,"{}",deliver,&delivered,180000,cancelled,&f)==200);
    assert(f.dial_budget==15000 && f.frees==1 && delivered==1);
    reset(500);assert(vela_https_post_json_stream_bounded("example.invalid","443","/test",NULL,"{}",deliver,&delivered,500,cancelled,&f)==VELA_TLS_ERR_PROTOCOL);
    assert(!f.dials && !f.frees);
    reset(0);assert(vela_https_post_json_stream("example.invalid","443","/test",NULL,"{}",deliver,&delivered)==200);
    assert(!f.dial_budget && f.frees==1 && delivered==2);
    mode="wifi";buffer="8192";wifi_up=true;errno=EINTR;
    zhaoxi_apply_wifi_tx_buffer(42);assert(changes==1 && actual==8192 && errno==EINTR);
    ppp_up=true;zhaoxi_apply_wifi_tx_buffer(42);assert(changes==1);ppp_up=false;
    wifi_up=false;zhaoxi_apply_wifi_tx_buffer(42);assert(changes==1);wifi_up=true;
    mode="pc";zhaoxi_apply_wifi_tx_buffer(42);assert(changes==1);mode="wifi";
    const char *bad[]={NULL,"0","1","65536","8192 ","garbage"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){buffer=bad[i];zhaoxi_apply_wifi_tx_buffer(42);assert(changes==1);}
    puts("PASS: actual speech JSON/SSE budgets, cancellation, cleanup, standard API compatibility and Wi-Fi-only opt-in queue bounds");
    return 0;
}

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define MBEDTLS_ERR_SSL_TIMEOUT -1
#define MBEDTLS_ERR_SSL_WANT_READ -2
#define MBEDTLS_ERR_SSL_WANT_WRITE -3
#define MBEDTLS_ERR_NET_RECV_FAILED -4
#define MBEDTLS_ERR_NET_SEND_FAILED -5
typedef struct {int fd;} mbedtls_net_context;
typedef struct {mbedtls_net_context net;int64_t request_deadline;
    int (*request_cancel)(void *);void *request_user;} tls_ctx_t;
static int mbedtls_net_recv(mbedtls_net_context *net,unsigned char *buf,size_t size)
{int n=recv(net->fd,buf,size,0);return n<0?MBEDTLS_ERR_NET_RECV_FAILED:n;}
static int mbedtls_net_send(mbedtls_net_context *net,const unsigned char *buf,size_t size)
{int n=send(net->fd,buf,size,0);return n<0?MBEDTLS_ERR_NET_SEND_FAILED:n;}
/* ACTUAL_BOUNDED_BIO */
static int cancel(void *flag){return *(int *)flag;}
int main(void)
{
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    tls_ctx_t ctx={.net={.fd=pair[0]}};unsigned char data[8192]={0};
    int64_t start=bounded_now_ms();ctx.request_deadline=start+1300;
    assert(bounded_recv(&ctx,data,1)==MBEDTLS_ERR_SSL_WANT_READ);
    assert(bounded_recv(&ctx,data,1)==MBEDTLS_ERR_SSL_TIMEOUT);
    int64_t elapsed=bounded_now_ms()-start;assert(elapsed>=1250 && elapsed<2300);
    int flag=1;ctx.request_deadline=bounded_now_ms()+30000;
    ctx.request_cancel=cancel;ctx.request_user=&flag;start=bounded_now_ms();
    assert(bounded_recv(&ctx,data,1)==MBEDTLS_ERR_SSL_TIMEOUT);
    assert(bounded_send(&ctx,data,1)==MBEDTLS_ERR_SSL_TIMEOUT);
    assert(bounded_now_ms()-start<100);flag=0;
    assert(!fcntl(pair[0],F_SETFL,O_NONBLOCK));
    while(send(pair[0],data,sizeof(data),0)>0){}
    assert(errno==EAGAIN || errno==EWOULDBLOCK);
    assert(!fcntl(pair[0],F_SETFL,0));start=bounded_now_ms();ctx.request_deadline=start+150;
    assert(bounded_send(&ctx,data,sizeof(data))==MBEDTLS_ERR_SSL_TIMEOUT);
    assert(bounded_now_ms()-start<1000);
    close(pair[0]);close(pair[1]);
    puts("PASS: actual background TLS BIO absolute deadline, repeated stalled reads, full send buffer and immediate cancellation");
    return 0;
}

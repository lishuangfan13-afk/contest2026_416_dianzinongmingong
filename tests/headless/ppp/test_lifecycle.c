/* SPDX-License-Identifier: Apache-2.0 */
/* Executes actual patched upstream pppd.c with explicit OS/PPP test doubles.
 * No UART, modem, Wi-Fi, or real network behavior is simulated as verified. */
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

static int fake_open(const char *, int, ...);
static int fake_close(int);
static int fake_fcntl(int, int, ...);
static int fake_ioctl(int, unsigned long, ...);
static ssize_t fake_read(int, void *, size_t);
static ssize_t fake_write(int, const void *, size_t);
static int fake_poll(struct pollfd *, nfds_t, int);
static unsigned fake_sleep(unsigned);
static int fake_clock_gettime(clockid_t, struct timespec *);
static int fake_usleep(useconds_t);
#ifdef PPP_FRAME_TX_TEST
static ssize_t test_frame_write(int,const void *,size_t);
static int test_frame_space(void);
static void test_frame_wait(void);
#endif
#define open fake_open
#define close fake_close
#define fcntl fake_fcntl
#define ioctl fake_ioctl
#define read fake_read
#define write fake_write
#define poll fake_poll
#define sleep fake_sleep
#define clock_gettime fake_clock_gettime
#define usleep fake_usleep
#include "pppd.c"
#undef open
#undef close
#undef fcntl
#undef ioctl
#undef read
#undef write
#undef poll
#undef sleep
#undef clock_gettime
#undef usleep

static int opened, closed, downs, writes, sleeps, disconnects, chats, inits, connects;
static size_t written_bytes;
static int fail_open, mode;
static bool stopping;
static time_t monotonic_seconds;
static unsigned monotonic_milliseconds;
static int ready_callbacks, negotiation_step;
static bool advance_poll_time;
struct fake_rx_s {
  unsigned char bytes[16384];
  size_t length, position, fragment, calls, maximum_request;
  int next_error;
  bool eof;
};
static struct fake_rx_s rx[2];
static void reset(void)
{
  opened=closed=downs=writes=sleeps=disconnects=chats=inits=connects=0;
  written_bytes=0;monotonic_milliseconds=0;
  stopping=false; fail_open=0; mode=0; monotonic_seconds=0;
  ready_callbacks=negotiation_step=0;
  advance_poll_time=false;
  memset(rx,0,sizeof(rx));
}
static int fake_open(const char *path, int flags, ...)
{ (void)path; (void)flags; return ++opened==fail_open ? -1 : 20+opened; }
static int fake_close(int fd) { assert(fd>=21 && fd<=22); closed++; return 0; }
static int fake_fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static int fake_ioctl(int fd, unsigned long cmd, ...)
{
  (void)fd;
  if(cmd==FIONSPACE) {
    va_list args;va_start(args,cmd);int *out=(int *)va_arg(args,unsigned long);va_end(args);
#ifdef PPP_FRAME_TX_TEST
    *out=test_frame_space();
#else
    *out=4094;
#endif
  }
  return 0;
}
static ssize_t fake_read(int fd, void *out, size_t size)
{
  if(fd<21 || fd>22){errno=EAGAIN;return -1;}
  struct fake_rx_s *input=&rx[fd-21];input->calls++;
  if(size>input->maximum_request)input->maximum_request=size;
  if(input->next_error){errno=input->next_error;input->next_error=0;return -1;}
  if(input->position==input->length){if(input->eof)return 0;errno=EAGAIN;return -1;}
  size_t count=input->length-input->position;
  if(count>size)count=size;
  if(input->fragment && count>input->fragment)count=input->fragment;
  memcpy(out,input->bytes+input->position,count);input->position+=count;return count;
}
static ssize_t fake_write(int fd, const void *out, size_t size)
{
  writes++;written_bytes+=size;
#ifdef PPP_FRAME_TX_TEST
  return test_frame_write(fd,out,size);
#else
  (void)fd;(void)out;return size;
#endif
}
static int fake_poll(struct pollfd *p, nfds_t n, int timeout)
{ (void)p; (void)n; if(advance_poll_time)monotonic_seconds+=timeout/1000;return 0; }
static unsigned fake_sleep(unsigned seconds) { sleeps++; return 0; }
static int fake_usleep(useconds_t microseconds)
{
  monotonic_milliseconds+=microseconds/1000;
  monotonic_seconds+=monotonic_milliseconds/1000;monotonic_milliseconds%=1000;
#ifdef PPP_FRAME_TX_TEST
  test_frame_wait();
#endif
  return 0;
}
static int fake_clock_gettime(clockid_t clock, struct timespec *out)
{ assert(clock==CLOCK_MONOTONIC);out->tv_sec=monotonic_seconds;out->tv_nsec=monotonic_milliseconds*1000000;return 0; }
size_t strlcpy(char *out,const char *in,size_t size)
{ size_t n=strlen(in); if(size){size_t c=n<size-1?n:size-1;memcpy(out,in,c);out[c]=0;}return n; }
int netlib_ifdown(const char *name) { assert(!strncmp(name,"ppp",3)); downs++; return 0; }
int chat(struct chat_ctl *ctl, const char *script)
{ (void)ctl; assert(script); chats++; return 0; }
bool zhaoxi_pc_link_stopping(void) { return stopping; }
void zhaoxi_pc_link_ipv4_ready(void) { ready_callbacks++; }
void ppp_init(struct ppp_context_s *ctx)
{
  inits++; ctx->ppp_flags=0; ctx->lcp_state=0;ctx->pc_link_last_rx=monotonic_seconds;
#ifdef PPP_UART_BUFFER_TEST
  ahdlc_init(ctx);ahdlc_rx_ready(ctx);
#endif
}
void ppp_connect(struct ppp_context_s *ctx) { connects++; ctx->ppp_flags=PPP_RX_READY; }
void lcp_disconnect(struct ppp_context_s *ctx, uint8_t id)
{ (void)ctx; (void)id; disconnects++; }
void ppp_send(struct ppp_context_s *ctx) { (void)ctx; }
#ifndef PPP_UART_BUFFER_TEST
void ppp_poll(struct ppp_context_s *ctx)
{
  if(mode==1) ctx->lcp_state |= LCP_TX_TIMEOUT;
  else if(mode==2) stopping=true;
  else if(mode==3) monotonic_seconds=63;
#ifdef PPP_FRAME_TX_TEST
  else if(mode==6) {
    ppp_arch_putchar(ctx,0x7e);ppp_arch_putchar(ctx,0x55);ppp_arch_putchar(ctx,0x7e);
  }
#endif
  else if(mode==4 || mode==5) {
    negotiation_step++;
    if(negotiation_step==1) {
      ctx->local_ip.s_addr=htonl(0x0afe2a02);
      ctx->ipcp_state=0; /* NAK has assigned an IP, no direction is open. */
      assert(ready_callbacks==0);
    } else if(negotiation_step==2) {
      ctx->ipcp_state=mode==4?IPCP_RX_UP:IPCP_TX_UP;
      assert(ready_callbacks==0);
    } else if(negotiation_step==3) {
      ctx->ipcp_state=IPCP_RX_UP|IPCP_TX_UP;
      assert(ready_callbacks==0);
    } else {
      assert(ready_callbacks==1);stopping=true;
    }
  }
  else abort();
}
#endif

int main(void)
{
  struct pppd_settings_s settings={.ttyname="/dev/console"};
  struct ppp_context_s ctx={.settings=&settings,.pc_link_managed=true};
  strcpy((char *)ctx.ifname,"ppp0");
  reset(); ppp_reconnect(&ctx);
  assert(inits==1 && connects==1 && writes==0 && sleeps==0 && disconnects==0);
  /* Subsequent direct-session reconnect becomes a visible terminal error. */
  ppp_reconnect(&ctx);
  assert(ctx.lcp_state & LCP_TX_TIMEOUT);
  assert(inits==1 && connects==1 && writes==0 && sleeps==0);

  reset(); mode=1;
  assert(pppd(&settings)==-ENETDOWN);
  assert(opened==2 && closed==2 && downs==2 && writes==0 && sleeps==0);
  reset(); mode=2;
  assert(pppd(&settings)==0);
  assert(opened==2 && closed==2 && downs==2);
  reset(); mode=4;
  assert(pppd(&settings)==0 && ready_callbacks==1);
  reset(); mode=5;
  assert(pppd(&settings)==0 && ready_callbacks==1);
  reset(); mode=3;
  assert(pppd(&settings)==-ENETDOWN);
  assert(opened==2 && closed==2 && downs==2);
  reset(); fail_open=1;
  assert(pppd(&settings)==2 && opened==1 && closed==0);
  reset(); fail_open=2;
  assert(pppd(&settings)==2 && opened==2 && closed==1);

  /* Explicit modem scripts retain upstream AT/disconnect behavior. */
  reset(); settings.connect_script="CONNECT";settings.disconnect_script="DISCONNECT";
  memset(&ctx,0,sizeof(ctx));ctx.settings=&settings;
  strcpy((char *)ctx.ifname,"ppp0");
  ppp_reconnect(&ctx);
  assert(writes==2 && sleeps==3 && disconnects==2 && chats==2);
  assert(inits==1 && connects==1);
  puts("PPP lifecycle: direct startup, failure cleanup, requested stop, idle receive deadline, NAK/one-direction not ready, both IPCP directions callback once, open failures, modem compatibility passed");
  return 0;
}

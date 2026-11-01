/* SPDX-License-Identifier: Apache-2.0 */
/* Actual PPP UART writer/AHDLC; models NuttX's ordinary-task write mutex. */
#define PPP_FRAME_TX_TEST
#define main lifecycle_tests_main
#include "test_lifecycle.c"
#undef main
#include "ahdlc.c"
void ppp_upcall(struct ppp_context_s *ctx,uint16_t p,uint8_t *b,uint16_t n)
{(void)ctx;(void)p;(void)b;(void)n;}

static unsigned char sink[32768];
static size_t sink_used,maximum_write;
static int space=4094,retry_error,retries,zeros,hard_error,short_count;
static unsigned waits,cancel_after;
static bool inject_logs;
static ssize_t test_frame_write(int fd,const void *data,size_t length)
{
  (void)fd;if(length>maximum_write)maximum_write=length;
  if(retries){retries--;errno=retry_error;return -1;}
  if(zeros){zeros--;return 0;}
  if(hard_error){errno=hard_error;return -1;}
  if(inject_logs){assert(sink_used+3<sizeof(sink));memcpy(sink+sink_used,"LOG",3);sink_used+=3;}
  size_t count=short_count>0 && (size_t)short_count<length?(size_t)short_count:length;
  assert(sink_used+count<=sizeof(sink));memcpy(sink+sink_used,data,count);sink_used+=count;
  return count;
}
static int test_frame_space(void){return space;}
static void test_frame_wait(void){waits++;if(cancel_after && waits>=cancel_after)stopping=true;}
static void fixture(struct ppp_context_s *ctx,bool managed)
{
  reset();memset(ctx,0,sizeof(*ctx));ctx->ctl.fd=21;ctx->pc_link_managed=managed;
  ctx->ppp_flags=PPP_RX_READY;ahdlc_init(ctx);ahdlc_rx_ready(ctx);
  sink_used=maximum_write=0;space=4094;retry_error=retries=zeros=hard_error=short_count=0;
  waits=cancel_after=0;inject_logs=false;
}
static int frame(struct ppp_context_s *ctx)
{
  assert(ppp_arch_putchar(ctx,0x7e)==1);assert(ppp_arch_putchar(ctx,0x55)==1);
  assert(!writes);return ppp_arch_putchar(ctx,0x7e);
}
int main(void)
{
  struct ppp_context_s a,b;fixture(&a,true);
  inject_logs=true;assert(frame(&a)==1 && writes==1 && sink_used==6);
  assert(!memcmp(sink,"LOG\x7e\x55\x7e",6));
  /* Separate frame/session buffers cannot splice their prefixes together. */
  fixture(&a,true);memset(&b,0,sizeof(b));b.ctl.fd=22;b.pc_link_managed=true;
  assert(ppp_arch_putchar(&a,0x7e));assert(ppp_arch_putchar(&a,0x41));
  assert(ppp_arch_putchar(&b,0x7e));assert(ppp_arch_putchar(&b,0x42));
  assert(ppp_arch_putchar(&b,0x7e));assert(ppp_arch_putchar(&a,0x7e));
  assert(writes==2 && !memcmp(sink,"\x7e\x42\x7e\x7e\x41\x7e",6));
  for(unsigned i=0;i<2;i++) {
    fixture(&a,true);retries=3;retry_error=i?EINTR:EAGAIN;
    assert(frame(&a)==1 && writes==4 && sink_used==3 && waits==3);
  }
  fixture(&a,true);zeros=3;assert(frame(&a)==1 && writes==4 && sink_used==3);
  fixture(&a,true);zeros=2000;
  assert(!frame(&a) && a.pc_tx_error==-ETIMEDOUT && !sink_used && waits==1000);
  fixture(&a,true);space=0;
  assert(!frame(&a) && a.pc_tx_error==-ETIMEDOUT && !writes && waits==1000);
  fixture(&a,true);space=0;cancel_after=3;
  assert(!frame(&a) && a.pc_tx_error==-ECANCELED && !writes && waits==3);
  fixture(&a,true);hard_error=EPIPE;
  assert(!frame(&a) && a.pc_tx_error==-EPIPE && writes==1 && !sink_used);
  fixture(&a,true);short_count=1;
  assert(!frame(&a) && a.pc_tx_error==-EIO && writes==1 && sink_used==1);
  for(unsigned i=0;i<100;i++)assert(!ppp_arch_putchar(&a,0x55));
  assert(writes==1 && sink_used==1); /* Never replay the transmitted prefix. */
  fixture(&a,true);
  assert(ppp_arch_putchar(&a,0x7e));
  for(size_t i=1;i<sizeof(a.pc_tx_buffer);i++)assert(ppp_arch_putchar(&a,0x55));
  assert(!ppp_arch_putchar(&a,0x55) && a.pc_tx_error==-EMSGSIZE && !writes);
  fixture(&a,true);assert(!ppp_arch_putchar(&a,0x55) && a.pc_tx_error==-EPROTO);
  fixture(&a,true);assert(ppp_arch_putchar(&a,0x7e));assert(ppp_arch_putchar(&a,0x41));
  a.ctl.fd=22;assert(ppp_arch_putchar(&a,0x7e));assert(ppp_arch_putchar(&a,0x42));assert(ppp_arch_putchar(&a,0x7e));
  assert(writes==1 && !memcmp(sink,"\x7e\x42\x7e",3));
  /* Maximum possible escaped PPP payload fits one write and preserves FCS. */
  fixture(&a,true);uint8_t payload[PPP_RX_BUFFER_SIZE];memset(payload,0,sizeof(payload));
  ahdlc_tx(&a,IPV4,NULL,payload,0,sizeof(payload));
  assert(writes==1 && maximum_write>2*sizeof(payload) && maximum_write<=sizeof(a.pc_tx_buffer));
  assert(sink[0]==0x7e && sink[sink_used-1]==0x7e);
  uint16_t fcs=0xffff;bool escaped=false;size_t bytes=0;
  for(size_t i=1;i+1<sink_used;i++) {
    uint8_t c=sink[i];if(!escaped && c==0x7d){escaped=true;continue;}
    if(escaped){c^=0x20;escaped=false;}fcs=crcadd(fcs,c);bytes++;
  }
  assert(!escaped && fcs==CRC_GOOD_VALUE && bytes==sizeof(payload)+6);
  /* Unmanaged modem still emits individual bytes with upstream behavior. */
  fixture(&a,false);ppp_arch_putchar(&a,0x7e);ppp_arch_putchar(&a,0x55);ppp_arch_putchar(&a,0x7e);
  assert(writes==3 && maximum_write==1);
  /* A sticky short-write failure reaches the actual owner cleanup path. */
  fixture(&a,true);mode=6;short_count=1;
  struct pppd_settings_s settings={.ttyname="/dev/console"};
  assert(pppd(&settings)==-EIO && closed==2 && writes==1 && sink_used==1);
  puts("PASS: actual PC PPP frame TX is one UART write despite competing ordinary logs; bounded EAGAIN/EINTR/zero/space/cancel, sticky short/error/overflow with no prefix replay, maximum escaped frame FCS, owner cleanup and modem isolation.");
  return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
/* Actual pppd RX, AHDLC decoder and ppp_poll with explicit UART/OS doubles. */
#define PPP_UART_BUFFER_TEST
#define main lifecycle_tests_main
#include "test_lifecycle.c"
#undef main
#include "ahdlc.c"

static unsigned received;
static unsigned char received_ids[256];
void ppp_upcall(struct ppp_context_s *ctx,uint16_t protocol,uint8_t *bytes,uint16_t length)
{
  assert(protocol==IPV4 && length==8 && received<sizeof(received_ids));
  received_ids[received++]=bytes[0];ctx->ip_len=length;
  memcpy(ctx->ip_buf,bytes,length);ctx->ip_no_data_time=0;
}
void lcp_echo_request(struct ppp_context_s *ctx){(void)ctx;}
void lcp_task(struct ppp_context_s *ctx,uint8_t *bytes){(void)ctx;(void)bytes;}
void ipcp_task(struct ppp_context_s *ctx,uint8_t *bytes){(void)ctx;(void)bytes;}
#include "ppp_poll_actual.h"

static void fixture_byte(struct fake_rx_s *input,uint8_t byte)
{
  if(byte<32 || byte==0x7d || byte==0x7e){input->bytes[input->length++]=0x7d;byte^=32;}
  input->bytes[input->length++]=byte;
  assert(input->length<sizeof(input->bytes));
}
static void fixture_frame(struct fake_rx_s *input,unsigned char id)
{
  uint8_t bytes[]={0xff,0x03,0x00,0x21,id,1,2,3,4,5,6,7};
  uint16_t crc=0xffff;input->bytes[input->length++]=0x7e;
  for(unsigned i=0;i<sizeof(bytes);i++){crc=crcadd(crc,bytes[i]);fixture_byte(input,bytes[i]);}
  crc^=0xffff;fixture_byte(input,crc&255);fixture_byte(input,crc>>8);
  input->bytes[input->length++]=0x7e;
}
static void ready(struct ppp_context_s *ctx,int fd)
{
  memset(ctx,0,sizeof(*ctx));ctx->ctl.fd=fd;ctx->pc_link_managed=true;
  strcpy((char *)ctx->ifname,"ppp0");
  ctx->ppp_flags=PPP_RX_READY;ctx->lcp_state=LCP_RX_UP|LCP_TX_UP;
  ctx->ipcp_state=IPCP_RX_UP|IPCP_TX_UP;
  ahdlc_init(ctx);ahdlc_rx_ready(ctx);
}
static void receive_frames(size_t fragment)
{
  reset();received=0;struct ppp_context_s ctx;ready(&ctx,21);
  rx[0].fragment=fragment;
  for(unsigned i=0;i<100;i++)fixture_frame(&rx[0],(unsigned char)i);
  size_t wire=rx[0].length;
  unsigned rounds=0;
  while(received<100){ppp_poll(&ctx);assert(++rounds<=100);}
  for(unsigned i=0;i<100;i++)assert(received_ids[i]==i);
  assert(rx[0].position==wire && !ctx.pc_rx_error);
  assert(rx[0].maximum_request==512);
  if(!fragment)assert(rx[0].calls<=(wire+511)/512);
  if(!fragment)printf("PPP buffered RX: %zu wire bytes in %zu read calls (100 complete frames)\n",wire,rx[0].calls);
}
int main(void)
{
  receive_frames(0);receive_frames(1);receive_frames(7);receive_frames(31);receive_frames(511);
  reset();received=0;struct ppp_context_s a,b;ready(&a,21);ready(&b,22);
  fixture_frame(&rx[0],11);fixture_frame(&rx[0],12);
  ppp_poll(&a);assert(received==1 && received_ids[0]==11);
  assert(a.pc_rx_position<a.pc_rx_length && ppp_uart_poll_timeout(&a)==0);
  size_t calls=rx[0].calls;
  ppp_poll(&a);assert(received==2 && received_ids[1]==12 && rx[0].calls==calls);
  assert(ppp_uart_poll_timeout(&a)==1000);
  /* Cached bytes are consumed before a later hard read failure. */
  reset();ready(&a,21);rx[0].bytes[0]='a';rx[0].bytes[1]='b';rx[0].length=2;
  uint8_t c;assert(ppp_arch_getchar(&a,&c)==1 && c=='a');rx[0].next_error=EIO;
  assert(ppp_arch_getchar(&a,&c)==1 && c=='b');assert(!a.pc_rx_error);
  assert(!ppp_arch_getchar(&a,&c) && a.pc_rx_error==-EIO);
  calls=rx[0].calls;assert(!ppp_arch_getchar(&a,&c) && rx[0].calls==calls);
  /* EAGAIN/EINTR and a raw UART zero-byte read remain recoverable. */
  for(unsigned i=0;i<2;i++){
    reset();ready(&a,21);rx[0].next_error=i?EINTR:EAGAIN;
    assert(!ppp_arch_getchar(&a,&c) && !a.pc_rx_error);
    rx[0].bytes[0]=42;rx[0].length=1;
    assert(ppp_arch_getchar(&a,&c)==1 && c==42);
  }
  reset();ready(&a,21);rx[0].eof=true;
  assert(!ppp_arch_getchar(&a,&c) && !a.pc_rx_error);
  rx[0].bytes[0]='r';rx[0].length=1;
  assert(ppp_arch_getchar(&a,&c)==1 && c=='r');
  /* Two contexts may each hold read-ahead bytes without crossing FDs. */
  reset();ready(&a,21);ready(&b,22);
  memcpy(rx[0].bytes,"abc",3);rx[0].length=3;
  memcpy(rx[1].bytes,"XYZ",3);rx[1].length=3;
  assert(ppp_arch_getchar(&a,&c)==1 && c=='a');
  assert(ppp_arch_getchar(&b,&c)==1 && c=='X');
  assert(ppp_arch_getchar(&a,&c)==1 && c=='b');
  assert(ppp_arch_getchar(&b,&c)==1 && c=='Y');
  /* Rebinding the same context cannot consume the previous FD's tail. */
  rx[1].position=0;memcpy(rx[1].bytes,"new",3);rx[1].length=3;
  a.ctl.fd=22;assert(ppp_arch_getchar(&a,&c)==1 && c=='n');
  assert(ppp_arch_getchar(&b,&c)==1 && c=='Z');
  /* Explicit reconnect resets owned state, and modem reads remain byte-wise. */
  struct pppd_settings_s settings={.ttyname="/dev/console"};
  a.settings=&settings;a.ppp_flags=0;a.pc_rx_error=-EIO;
  a.pc_rx_position=0;a.pc_rx_length=2;ppp_reconnect(&a);
  assert(!a.pc_rx_error && !a.pc_rx_length && !a.pc_rx_position);
  reset();ready(&a,21);a.pc_link_managed=false;
  memcpy(rx[0].bytes,"modem",5);rx[0].length=5;
  assert(ppp_arch_getchar(&a,&c)==1 && c=='m' && rx[0].maximum_request==1);
  assert(ppp_uart_poll_timeout(&a)==1000);
  /* Requested stop does not refill from the UART; owner still closes FDs. */
  reset();ready(&a,21);stopping=true;rx[0].bytes[0]='x';rx[0].length=1;
  assert(!ppp_arch_getchar(&a,&c) && !rx[0].calls && !a.pc_rx_error);
  /* Raw-UART emptiness uses the existing inactivity bound, not false EOF. */
  reset();received=0;rx[1].eof=true;advance_poll_time=true;
  assert(pppd(&settings)==-ENETDOWN && closed==2);
  reset();received=0;rx[1].next_error=EIO;
  assert(pppd(&settings)==-EIO && closed==2);
  reset();received=0;fixture_frame(&rx[1],99);fixture_frame(&rx[1],100);rx[1].eof=true;advance_poll_time=true;
  assert(pppd(&settings)==-ENETDOWN && closed==2 && received==2);
  assert(received_ids[0]==99 && received_ids[1]==100);
  puts("PASS: actual PC UART buffered RX, short reads/multiframe/frame boundaries, independent sessions, rebind, temporary errors/zero-read recovery/stop cleanup, modem isolation.");
  return 0;
}

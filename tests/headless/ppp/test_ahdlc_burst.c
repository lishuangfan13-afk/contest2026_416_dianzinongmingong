/* SPDX-License-Identifier: Apache-2.0 */
/* Replay actual AHDLC encoder/decoder and patched ppp_reconnect with a
 * controlled monotonic clock. This verifies deadlines and short bursts;
 * it does not verify real UART throughput or hardware connectivity. */
#define main lifecycle_tests_main
#include "test_lifecycle.c"
#undef main
#include "ahdlc.c"

static int received;
void ppp_upcall(struct ppp_context_s *ctx, uint16_t protocol,
                uint8_t *buffer, uint16_t length)
{
  (void)ctx; (void)buffer;
  assert(protocol==LCP && length==8);
  received++;
}
static void receive_byte(struct ppp_context_s *ctx, uint8_t byte)
{
  if(byte<0x20 || byte==0x7d || byte==0x7e) {
    assert(ahdlc_rx(ctx,0x7d)==0);byte^=0x20;
  }
  assert(ahdlc_rx(ctx,byte)==0);
}
static void peer_reply_frame(struct ppp_context_s *ctx, bool corrupt)
{
  /* LCP Echo-Reply: a complete frame with its actual PPP FCS. */
  uint8_t packet[]={0xff,0x03,0xc0,0x21,0x0a,1,0,8,0,0,0,0};
  uint16_t fcs=0xffff;
  assert(ahdlc_rx(ctx,0x7e)==0);
  for(unsigned i=0;i<sizeof(packet);i++) {
    fcs=crcadd(fcs,packet[i]);receive_byte(ctx,packet[i]);
  }
  fcs^=0xffff;
  if(corrupt)fcs^=1;
  receive_byte(ctx,fcs&0xff);receive_byte(ctx,fcs>>8);
  assert(ahdlc_rx(ctx,0x7e)==0);
}
static void peer_reply(struct ppp_context_s *ctx) { peer_reply_frame(ctx,false); }
static void ready(struct ppp_context_s *ctx,const struct pppd_settings_s *settings)
{
  memset(ctx,0,sizeof(*ctx));ctx->settings=settings;
  strcpy((char *)ctx->ifname,"ppp0");
  ctx->pc_link_managed=true;
  ctx->ppp_flags=PPP_RX_READY;
  ctx->lcp_state=LCP_TX_UP|LCP_RX_UP;
  ahdlc_init(ctx);ahdlc_rx_ready(ctx);
}
int main(void)
{
  struct pppd_settings_s settings={.ttyname="/dev/console"};
  struct ppp_context_s ctx;
  uint8_t data[512];memset(data,0x55,sizeof(data));
  reset();ready(&ctx,&settings);received=0;
  peer_reply(&ctx);
  assert(received==1 && ctx.ahdlc_tx_offline==0);
  for(int i=0;i<6;i++)ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  assert(downs==0 && ctx.ppp_tx_frame_count==6);
  double serialized_ms=(double)written_bytes*10.0*1000.0/921600.0;
  assert(serialized_ms<100.0);
  /* A healthy peer can take 100 ms to reply. A seven-frame burst must not
   * disconnect simply because the next response is still in flight. */
  ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  assert(downs==0 && !(ctx.lcp_state&LCP_TX_TIMEOUT));
  assert(ctx.ppp_tx_frame_count==7);
  printf("PASSED: seven-frame 512-byte burst continues (first six %.2f ms wire estimate)\n",serialized_ms);
  peer_reply(&ctx);
  assert(received==2 && !(ctx.lcp_state&LCP_TX_TIMEOUT));

  monotonic_seconds=62;
  peer_reply_frame(&ctx,true);
  assert(received==2 && ctx.pc_link_last_rx==0);
  ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  assert(downs==0 && !ppp_pc_link_expired(&ctx));
  monotonic_seconds=63;
  assert(ppp_pc_link_expired(&ctx));
  puts("PASSED: no valid RX for 63 seconds expires the direct session (main-loop exit tested separately)");

  reset();ready(&ctx,&settings);received=0;
  for(int i=0;i<6;i++)ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  monotonic_seconds=62;
  peer_reply(&ctx); /* timing of response alone changes the outcome */
  monotonic_seconds=100;
  ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  assert(downs==0 && !(ctx.lcp_state&LCP_TX_TIMEOUT));
  assert(ctx.ppp_tx_frame_count==7 && received==1);
  monotonic_seconds=124;
  ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  assert(downs==0 && !ppp_pc_link_expired(&ctx));
  monotonic_seconds=125;
  assert(ppp_pc_link_expired(&ctx));
  puts("PASSED: valid peer replies renew the 63-second receive deadline");

  reset();ready(&ctx,&settings);ctx.pc_link_managed=false;
  for(int i=0;i<7;i++)ahdlc_tx(&ctx,IPV4,NULL,data,0,sizeof(data));
  assert(downs==1 && disconnects==2 && sleeps==3);
  assert(ctx.ppp_tx_frame_count==6);
  puts("PASSED: unmanaged modem retains the original frame-count reconnect path");
  return 0;
}

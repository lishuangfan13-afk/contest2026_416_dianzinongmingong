/* SPDX-License-Identifier: Apache-2.0 */
/* Real upstream LCP and AHDLC; UART output and unrelated IPCP are doubles. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define UNUSED(x) ((void)(x))
#include "ppp.h"

static unsigned char wire[4096],decoded[2048],delivered[1024];
static size_t wire_used,decoded_used,delivered_used;
static unsigned upcalls;
static uint16_t delivered_protocol;
static time_t now=100;
time_t ppp_arch_clock_seconds(void){return now;}
int ppp_arch_putchar(struct ppp_context_s *ctx,uint8_t byte)
{
  (void)ctx;assert(wire_used<sizeof(wire));wire[wire_used++]=byte;return 1;
}
void ppp_reconnect(struct ppp_context_s *ctx){(void)ctx;assert(!"Unexpected reconnect");}
void ipcp_init(struct ppp_context_s *ctx){ctx->ipcp_state=0;}
uint16_t scan_packet(struct ppp_context_s *ctx,uint16_t protocol,const uint8_t *list,
                     uint8_t *buffer,uint8_t *options,uint16_t length)
{
  (void)ctx;(void)protocol;(void)list;(void)buffer;(void)options;(void)length;
  return 0; /* Fixtures contain a single known, well-formed ACCM option. */
}
void ppp_upcall(struct ppp_context_s *ctx,uint16_t protocol,uint8_t *data,uint16_t length)
{
  (void)ctx;assert(length<=sizeof(delivered));
  delivered_protocol=protocol;delivered_used=length;memcpy(delivered,data,length);upcalls++;
}
#include "ahdlc.c"
#include "lcp.c"

static void ready(struct ppp_context_s *ctx,bool pc)
{
  memset(ctx,0,sizeof(*ctx));ctx->pc_link_managed=pc;ctx->ppp_flags=PPP_RX_READY;
  ahdlc_init(ctx);ahdlc_rx_ready(ctx);lcp_init(ctx);wire_used=0;
}
static void decode_frame(void)
{
  assert(wire_used>=8 && wire[0]==0x7e && wire[wire_used-1]==0x7e);
  decoded_used=0;bool escaped=false;uint16_t fcs=0xffff;
  for(size_t i=1;i+1<wire_used;i++) {
    uint8_t byte=wire[i];
    if(!escaped && byte==0x7d){escaped=true;continue;}
    if(escaped){byte^=0x20;escaped=false;}
    assert(decoded_used<sizeof(decoded));decoded[decoded_used++]=byte;fcs=crcadd(fcs,byte);
  }
  assert(!escaped && fcs==CRC_GOOD_VALUE && decoded_used>=6);
  assert(decoded[0]==0xff && decoded[1]==0x03);
}
static void check_advertisement(bool pc)
{
  struct ppp_context_s ctx;uint8_t scratch[128];ready(&ctx,pc);
  lcp_task(&ctx,scratch);decode_frame();
  assert(decoded[2]==0xc0 && decoded[3]==0x21 && decoded[4]==CONF_REQ);
  assert(decoded[8]==LPC_ACCM && decoded[9]==6);
  for(unsigned i=10;i<14;i++)assert(decoded[i]==(pc?0:255));
  /* LCP is escaped even when advertising ACCM=0. */
  for(size_t i=1;i+1<wire_used;i++)assert(wire[i]>=0x20);
}
static void peer_map(struct ppp_context_s *ctx,uint8_t map)
{
  uint8_t request[]={CONF_REQ,7,0,10,LPC_ACCM,6,map,map,map,map};
  wire_used=0;ctx->ahdlc_tx_offline=0;lcp_rx(ctx,request,sizeof(request));decode_frame();
  assert(decoded[4]==CONF_ACK && (ctx->lcp_state&LCP_RX_UP));
  assert(!!(ctx->ahdlc_flags&PPP_TX_ASYNC_MAP)==(map==0));
}
static size_t all_bytes(bool zero_map)
{
  struct ppp_context_s tx,rx;ready(&tx,true);ready(&rx,true);
  peer_map(&tx,zero_map?0:255);
  unsigned char payload[512];for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)i;
  wire_used=0;assert(ahdlc_tx(&tx,IPV4,NULL,payload,0,sizeof(payload))==0);
  decode_frame();assert(decoded_used==sizeof(payload)+6);
  assert(decoded[2]==0 && decoded[3]==0x21 && !memcmp(decoded+4,payload,sizeof(payload)));
  if(zero_map) {
    bool raw_xon=false,raw_xoff=false,raw_nul=false;
    for(size_t i=1;i+1<wire_used;i++) {
      if(wire[i]==0x11)raw_xon=true;
      if(wire[i]==0x13)raw_xoff=true;
      if(!wire[i])raw_nul=true;
    }
    assert(raw_xon && raw_xoff && raw_nul);
  } else {
    for(size_t i=1;i+1<wire_used;i++)assert(wire[i]>=0x20);
  }
  upcalls=0;
  for(size_t i=0;i<wire_used;i++)assert(ahdlc_rx(&rx,wire[i])==0);
  assert(upcalls==1 && delivered_protocol==IPV4 && delivered_used==sizeof(payload));
  assert(!memcmp(delivered,payload,sizeof(payload)) && !rx.ahdlc_crc_error);
  /* A corrupted payload must not pass CRC or reach the network layer. */
  bool changed=false;
  for(size_t i=1;i+1<wire_used;i++)if(wire[i]==0x55){wire[i]^=1;changed=true;break;}
  assert(changed);
  for(size_t i=0;i<wire_used;i++)assert(ahdlc_rx(&rx,wire[i])==0);
  assert(upcalls==1 && rx.ahdlc_crc_error==1);
  return wire_used;
}
int main(void)
{
  check_advertisement(true);check_advertisement(false);
  struct ppp_context_s ctx;ready(&ctx,true);peer_map(&ctx,0);peer_map(&ctx,255);peer_map(&ctx,0);
  size_t raw=all_bytes(true),full=all_bytes(false);
  assert(raw<full);
  printf("PASS: actual LCP PC ACCM=0/modem=ffffffff, peer TX-map changes, LCP escaping; actual AHDLC 0..255 TX/RX exact and corrupt CRC rejected (raw=%zu default=%zu bytes).\n",raw,full);
  return 0;
}

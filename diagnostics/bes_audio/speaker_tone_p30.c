#include <stdint.h>
#include <stddef.h>
extern int printf(const char *,...);
extern void *malloc(size_t);
extern void free(void *);
extern unsigned long strtoul(const char *,char **,int);
extern int usleep(unsigned),clock_gettime(int,void *);
extern int codec_int_open(void),codec_int_close(void);
extern int codec_int_stream_open(unsigned),codec_int_stream_close(unsigned);
extern int codec_int_stream_setup(unsigned,const void *),codec_int_stream_start(unsigned),codec_int_stream_stop(unsigned);
extern unsigned analog_codec_get_dac_dev(void);
extern int hal_analogif_reg_read(unsigned,uint16_t *);
extern uint8_t hal_dma_get_chan(unsigned,unsigned);
extern int hal_dma_start(const void *),hal_dma_chan_busy(uint8_t);
extern void hal_dma_free_chan(uint8_t),hal_dma_sync_for_device(const void *,unsigned);
extern unsigned hal_dma_cancel(uint8_t);
struct ccfg {uint32_t bits,rate,channels,map;uint8_t dma_vol,path,pad[2];uint32_t flags;};
struct dcfg {uint8_t ch,burst;uint16_t count;uint8_t sw,dw,sb,db,flow,sp,dp,pad;uint32_t src,dst,handler,start;};
struct stamp {int64_t sec;int32_t ns,pad;};
_Static_assert(sizeof(struct dcfg)==28,"DMA ABI");
#define RESULT ((volatile unsigned *)0x18009d00)
static unsigned now_ms(void){struct stamp t;clock_gettime(1,&t);return (unsigned)t.sec*1000u+(unsigned)t.ns/1000000u;}
static void complete(uint8_t ch,unsigned remain,unsigned error,void *next)
{(void)ch;(void)next;RESULT[1]=remain;RESULT[2]=error;RESULT[0]=1;}
static void regs(unsigned stage)
{
  static const uint16_t addresses[]={0x1006,0x1009,0x100d,0x1011};
  for(unsigned i=0;i<4;i++){uint16_t v=0xffff;int e=hal_analogif_reg_read(addresses[i],&v);printf("DACREG stage=%u addr=%04x value=%04x ret=%d\n",stage,addresses[i],v,e);}
}
__attribute__((section(".entry"))) int probe_entry(int argc,char **argv)
{
  if(argc<2 || argv[1][0]!='d'){printf("SPKPROBE2 usage: codecprobe dac [100..2000ms] [128..8192 peak]; manual only\n");return -1;}
  unsigned ms=argc>2?strtoul(argv[2],0,0):100;
  unsigned peak=argc>3?strtoul(argv[3],0,0):128;
  if(ms<100||ms>2000||peak<128||peak>8192)return -1;
  struct ccfg c={16,16000,1,1,3,0,{0,0},0};
  struct dcfg d={0,0,0,1,1,1,1,1,0,71,0,0,0,0,0};
  unsigned dev=analog_codec_get_dac_dev(),phase=0,n=0,begin=0;
  uint8_t ch=255;int ret=-1;void *mem=0;int16_t *buf;
  printf("SPKPROBE device=%u tone=1000Hz pcm_peak=%u digital_gain=0dB ms=%u\n",dev,peak,ms);
  /* Route 4 is selected by this board's factory analog_open for PMU type 6. */
  if(dev!=4)return -1;
  regs(0);
  mem=malloc(3200+64);if(!mem)return -1;
  buf=(int16_t *)(((uintptr_t)mem+63)&~(uintptr_t)63);
  static const int16_t wave16[]={0,196,362,473,512,473,362,196,0,-196,-362,-473,-512,-473,-362,-196};
  for(unsigned j=0;j<1600;j++)buf[j]=(wave16[j&15]*(int)peak)/512;
#define CHECK(x) do{ret=(x);if(ret){printf("DACPROBE fail line=%d ret=%d\n",__LINE__,ret);goto finish;}}while(0)
  CHECK(codec_int_open());phase=1;
  CHECK(codec_int_stream_open(0));phase=2;
  CHECK(codec_int_stream_setup(0,&c));
  /* Exact original ELF: hal_codec_set_dig_dac_gain(uint32_t channel_map,int db). */
  ((void(*)(unsigned,int))0x101ece81)(1,-80);
  ch=hal_dma_get_chan(71,0);if(ch==255){ret=-1;goto finish;}
  d.ch=ch;d.src=(uint32_t)buf;d.handler=(uint32_t)complete;
  hal_dma_sync_for_device(buf,3200);
  CHECK(codec_int_stream_start(0));phase=3;
  ((void(*)(unsigned,int))0x101ece81)(1,0);
  unsigned gain=*(volatile unsigned *)0x503000b4 & 0xfffff;
  printf("SPKPROBE actual_dac_gain=%u expected=16384\n",gain);
  if(gain!=16384){ret=-1;goto finish;}
  regs(1);begin=now_ms();
  while(n<ms*16u){
    unsigned count=ms*16u-n;if(count>1600)count=1600;d.count=count;
    RESULT[0]=0;RESULT[1]=RESULT[2]=0xffffffff;
    CHECK(hal_dma_start(&d));unsigned start=now_ms();
    while(!RESULT[0] && now_ms()-start<500){}
    if(!RESULT[0] || RESULT[1] || RESULT[2] || hal_dma_chan_busy(ch)){
      printf("DACPROBE incomplete done=%u remain=%u err=%u\n",RESULT[0],RESULT[1],RESULT[2]);ret=-1;goto finish;
    }
    n+=count;
  }
  printf("SPKPROBE samples=%u elapsed=%u gain_code=%u\n",n,now_ms()-begin,*(volatile unsigned *)0x503000b4 & 0xfffff);
  ret=0;
finish:
  if(phase>=2)((void(*)(unsigned,int))0x101ece81)(1,-80);
  if(ch!=255)hal_dma_cancel(ch);
  if(phase>=3)codec_int_stream_stop(0);
  if(phase>=2)codec_int_stream_close(0);
  if(phase>=1)codec_int_close();
  if(ch!=255)hal_dma_free_chan(ch);
  free(mem);regs(2);printf("DACPROBE done ret=%d\n",ret);return ret;
}

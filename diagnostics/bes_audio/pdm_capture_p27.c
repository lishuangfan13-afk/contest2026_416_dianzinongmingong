#include <stdint.h>
#include <stddef.h>
extern int printf(const char *,...);
extern void *malloc(size_t);
extern void free(void *);
extern int open(const char *,int,...),write(int,const void *,unsigned),close(int),usleep(unsigned),clock_gettime(int,void *);
extern unsigned long strtoul(const char *,char **,int);
extern int codec_int_open(void),codec_int_close(void);
extern int codec_int_stream_open(unsigned),codec_int_stream_close(unsigned),codec_int_stream_setup(unsigned,const void *),codec_int_stream_start(unsigned),codec_int_stream_stop(unsigned);
extern void hal_codec_ci2s_rx_enable(unsigned);
extern int bes_enable_mic_regulator(int);
extern uint8_t hal_dma_get_chan(unsigned,unsigned);
extern int hal_dma_start(const void *),hal_dma_chan_busy(uint8_t);
extern void hal_dma_free_chan(uint8_t);
extern unsigned hal_dma_cancel(uint8_t),hal_dma_get_cur_dst_addr(uint8_t);
extern void hal_dma_sync_for_cpu(const void *,unsigned),hal_dma_sync_for_device(const void *,unsigned);
/* Byte 16 is packed: use_dma bit 0, volume bits 1..7. */
struct ccfg { uint32_t bits,rate,channels,map; uint8_t dma_vol,path,pad[2]; uint32_t flags; };
/* Packed enum offsets recovered from this exact best1700 HAL disassembly. */
struct dcfg { uint8_t ch,burst; uint16_t count; uint8_t sw,dw,sb,db,flow,sp,dp,pad; uint32_t src,dst,handler,start; };
_Static_assert(sizeof(struct dcfg)==28,"DMA ABI");
_Static_assert(offsetof(struct dcfg,dst)==16,"DMA ABI");
struct stamp { int64_t sec; int32_t ns,pad; };
static unsigned now_ms(void){struct stamp t;clock_gettime(1,&t);return (unsigned)t.sec*1000u+(unsigned)t.ns/1000000u;}
/* Reserved g_cap from the original diagnostic, unused by this replacement. */
#define DMA_RESULT ((volatile unsigned *)0x18009d00)
static void dma_done(uint8_t ch,unsigned remain,unsigned error,void *next)
{(void)ch;(void)next;DMA_RESULT[1]=remain;DMA_RESULT[2]=error;DMA_RESULT[0]=1;}
__attribute__((section(".entry"))) int probe_entry(int argc,char **argv)
{
  struct ccfg c={16,16000,1,1,33,1,{0,0},0};
  struct dcfg d={0,0,0,1,1,1,1,2,70,0,0,0,0,0,0};
  volatile uint32_t *r=(volatile uint32_t *)0x50300000;
  unsigned ms=argc>2?strtoul(argv[2],0,0):100;
  unsigned map=argc>1?strtoul(argv[1],0,0):4096;
  unsigned total,n=0,phase=0,powered=0,begin=0,elapsed=0,changed=0,nz[2]={0,0};
  int min[2]={32767,32767},max[2]={-32768,-32768},ret=-1,fd,wrote;
  uint8_t channel=255;
  void *allocation;
  int16_t *buf;
  if(map!=4096 && map!=8192){printf("CODECDMA usage: codecprobe 4096|8192 [ms 100..5000] (PDM GPIO82/83)\n");return -1;}
  if(ms<100||ms>5000)return -1;
  c.map=map;
  total=ms*16u;
  allocation=malloc(total*2u+64);
  if(!allocation)return -1;
  buf=(int16_t *)(((uintptr_t)allocation+63)&~(uintptr_t)63);
  for(unsigned j=0;j<total;j++)buf[j]=0x5a5a;
  printf("CODECDMIC1 mono map=%u ms=%u samples=%u\n",map,ms,total);
#define CHECK(x) do{ret=(x);if(ret){printf("CODECDMA fail line=%d ret=%d\n",__LINE__,ret);goto finish;}}while(0)
  /* V1.3 digital microphones use VBUCK_1V7, not the analog VMIC regulator. */
  CHECK(codec_int_open());phase=1;
  CHECK(codec_int_stream_open(1));phase=2;
  CHECK(codec_int_stream_setup(1,&c));
  phase=3;
  channel=hal_dma_get_chan(70,0);printf("CODECDMA channel=%u\n",channel);
  if(channel==255){ret=-1;goto finish;}
  d.ch=channel;
  d.handler=(uint32_t)dma_done;
  CHECK(codec_int_stream_start(1));phase=4;
  usleep(100000);
  r[1]|=3;usleep(1000);r[1]&=~3u;
  printf("CODECDMA ctrl=%08x count=%08x errors_before=%08x\n",r[0],r[5],r[3]);
  begin=now_ms();
  while(n<total){
    unsigned size=total-n>1600?1600:total-n;
    d.count=size;d.dst=(uint32_t)(buf+n);
    DMA_RESULT[0]=0;DMA_RESULT[1]=0xffffffff;DMA_RESULT[2]=0xffffffff;
    hal_dma_sync_for_device(buf+n,size*2u);
    CHECK(hal_dma_start(&d));
    unsigned started=now_ms();
    while(!DMA_RESULT[0] && now_ms()-started<500){}
    unsigned end=hal_dma_get_cur_dst_addr(channel);
    if(!DMA_RESULT[0] || DMA_RESULT[1] || DMA_RESULT[2] || hal_dma_chan_busy(channel)){
      printf("CODECDMA incomplete done=%u remain=%u error=%u dst=%08x\n",DMA_RESULT[0],DMA_RESULT[1],DMA_RESULT[2],end);ret=-1;goto finish;
    }
    hal_dma_sync_for_cpu(buf+n,size*2u);
    n+=size;
  }
  elapsed=now_ms()-begin;
  printf("CODECDMA samples=%u elapsed=%u count=%08x errors_after=%08x\n",n,elapsed,r[5],r[3]);
  for(unsigned j=0;j<n;j++){
    int v=buf[j];unsigned ch=0;
    if(v<min[ch])min[ch]=v;if(v>max[ch])max[ch]=v;if(v)nz[ch]++;if(v!=0x5a5a)changed++;
  }
  printf("CODECDMA changed=%u mono=%d..%d nz=%u\n",changed,min[0],max[0],nz[0]);
  fd=open("/tmp/codex_dma.pcm",0x26,0666);wrote=fd<0?-1:write(fd,buf,n*2u);if(fd>=0)close(fd);
  printf("CODECDMA file=/tmp/codex_dma.pcm bytes=%u written=%d fs=16000 ch=1 bits=16\n",n*2u,wrote);
  ret=(changed>0 && wrote==(int)n*2)?0:-1;
finish:
  if(channel!=255)hal_dma_cancel(channel);
  if(phase>=4)codec_int_stream_stop(1);
  if(phase>=2)codec_int_stream_close(1);
  if(phase>=1)codec_int_close();
  if(channel!=255)hal_dma_free_chan(channel);
  if(powered)bes_enable_mic_regulator(0);
  free(allocation);printf("CODECDMA done ret=%d\n",ret);return ret;
}

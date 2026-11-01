#!/usr/bin/env python3
"""Compile actual BES DMA IRQ/wait code with deterministic HAL substitutes."""
from pathlib import Path
import argparse,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[3]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True)
    sdk=p.parse_args().sdk
    with tempfile.TemporaryDirectory(prefix='bes-dma-irq-') as folder:
        root=Path(folder);source=root/'test_dma.c';exe=root/'test_dma'
        source.write_text(PRELUDE+'\n#include "'+str(ROOT/'app/zhaoxi/bes_audio.c')+'"\n'+LEGACY+TESTS)
        subprocess.run(['gcc','-D_GNU_SOURCE','-std=gnu11','-O1','-g','-pthread','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections',
            '-Wl,--gc-sections','-I'+str(sdk/'src'),'-I'+str(sdk/'include'),str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)

# Historical defect reproducer from 2dc773e; independent of the current HEAD.
LEGACY=r'''
static int legacy_transfer(struct pcm_session *s,unsigned samples)
{
 if(atomic_load(&s->aborted))return -ECANCELED;
 s->dma.count=samples;
 if(s->direction)s->dma.dst=(uint32_t)(uintptr_t)s->buffer;
 else s->dma.src=(uint32_t)(uintptr_t)s->buffer;
 struct completion *done=&completion[s->direction];
 done->remain=done->error=~0u;atomic_store(&done->done,false);
 hal_dma_sync_for_device(s->buffer,samples*2);
 if(hal_dma_start(&s->dma))return -EIO;
 unsigned start=milliseconds();
 while(!atomic_load(&done->done)) {
  if(atomic_load(&s->aborted)){hal_dma_cancel(s->channel);return -ECANCELED;}
  if(milliseconds()-start>750){hal_dma_cancel(s->channel);return -ETIMEDOUT;}
  usleep(500);
 }
 if(done->remain || done->error || hal_dma_chan_busy(s->channel))return -EIO;
 if(s->direction)hal_dma_sync_for_cpu(s->buffer,samples*2);
 return 0;
}
'''

PRELUDE=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int fixture_sleep(useconds_t);
static int fixture_clockwait(sem_t *,clockid_t,const struct timespec *);
static int fixture_sem_init(sem_t *,int,unsigned);
#define usleep fixture_sleep
#define sem_clockwait fixture_clockwait
#define sem_init fixture_sem_init
'''
TESTS=r'''
#undef usleep
#undef sem_clockwait
#undef sem_init
static atomic_int busy,cancel_count,sleep_count,init_count;
static int mode,direction,remain_value,error_value,legacy_mode,interrupt_wait;
static pthread_t irq_thread;
static int irq_live;
static struct timespec saved_deadline;
static int deadline_saved;
enum {ASYNC=0,IMMEDIATE,NEVER,START_FAIL};
static void delay_us(unsigned value)
{struct timespec t={value/1000000,(long)(value%1000000)*1000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static int fixture_sleep(useconds_t value)
{atomic_fetch_add(&sleep_count,1);delay_us(((value+9999)/10000)*10000);return 0;}
static int fixture_sem_init(sem_t *sem,int shared,unsigned value)
{atomic_fetch_add(&init_count,1);return sem_init(sem,shared,value);}
static int fixture_clockwait(sem_t *sem,clockid_t clock,const struct timespec *deadline)
{
 assert(clock==CLOCK_MONOTONIC);
 if(!deadline_saved){saved_deadline=*deadline;deadline_saved=1;}
 else assert(saved_deadline.tv_sec==deadline->tv_sec && saved_deadline.tv_nsec==deadline->tv_nsec);
 if(interrupt_wait){interrupt_wait--;errno=EINTR;return -1;}
 return sem_clockwait(sem,clock,deadline);
}
static void complete(void)
{
 atomic_store(&busy,0);
 if(direction)capture_done(4,remain_value,error_value,NULL);
 else playback_done(4,remain_value,error_value,NULL);
}
static void *irq(void *unused){(void)unused;delay_us(1000);complete();return NULL;}
int hal_dma_start(const void *config)
{
 assert(((const struct dma_config *)config)->count>0);
 if(mode==START_FAIL)return 1;
 atomic_store(&busy,1);
 if(legacy_mode)atomic_store(&completion[direction].armed,true);
 if(mode==IMMEDIATE)complete();
 else if(mode==ASYNC){assert(!pthread_create(&irq_thread,NULL,irq,NULL));irq_live=1;}
 return 0;
}
int hal_dma_chan_busy(uint8_t channel){assert(channel==4);return atomic_load(&busy);}
unsigned hal_dma_cancel(uint8_t channel)
{assert(channel==4);atomic_fetch_add(&cancel_count,1);atomic_store(&busy,0);return 0;}
void hal_dma_sync_for_device(const void *p,unsigned n){assert(p && n>0);}
void hal_dma_sync_for_cpu(const void *p,unsigned n){assert(p && n>0);}
static int64_t now_us(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void reset(unsigned d)
{
 if(irq_live){pthread_join(irq_thread,NULL);irq_live=0;}
 direction=d;mode=ASYNC;remain_value=error_value=legacy_mode=interrupt_wait=deadline_saved=0;
 atomic_store(&busy,0);atomic_store(&cancel_count,0);atomic_store(&sleep_count,0);
}
struct call {struct pcm_session *s;int result;};
static void *call_transfer(void *arg){struct call *c=arg;c->result=transfer(c->s,1600);return NULL;}
int main(void)
{
 assert(!pthread_once(&completion_once,completion_init));assert(!completion_init_error);
 assert(atomic_load(&init_count)==2);
 int16_t pcm[1600]={0};struct pcm_session s={.buffer=pcm,.channel=4,.direction=0,.phase=3};
 reset(0);legacy_mode=1;int64_t begin=now_us();
 for(int i=0;i<8;i++){assert(!legacy_transfer(&s,1600));pthread_join(irq_thread,NULL);irq_live=0;}
 int64_t old_us=now_us()-begin;assert(atomic_load(&sleep_count)>=8);
 reset(0);begin=now_us();
 for(int i=0;i<8;i++){deadline_saved=0;assert(!transfer(&s,1600));pthread_join(irq_thread,NULL);irq_live=0;}
 int64_t new_us=now_us()-begin;assert(!atomic_load(&sleep_count));assert(new_us<old_us*3/4);
 printf("DMA 8x1ms: old 10ms-tick poll=%lldus IRQ wait=%lldus\n",(long long)old_us,(long long)new_us);

 reset(0);mode=IMMEDIATE;assert(!transfer(&s,1600)); /* IRQ before wait. */
 reset(1);s.direction=1;interrupt_wait=1;assert(!transfer(&s,1600));reset(0);s.direction=0;
 mode=START_FAIL;assert(transfer(&s,1600)==-EIO && !atomic_load(&completion[0].armed));
 reset(0);mode=IMMEDIATE;remain_value=1;assert(transfer(&s,1600)==-EIO);
 reset(0);mode=IMMEDIATE;error_value=2;assert(transfer(&s,1600)==-EIO);

 /* A stale post is drained; it cannot impersonate a missing completion. */
 reset(0);mode=NEVER;sem_post(&completion[0].signal);begin=now_us();
 assert(transfer(&s,1600)==-ETIMEDOUT);
 assert(now_us()-begin>=700000 && now_us()-begin<1000000 && atomic_load(&cancel_count)==1);
 playback_done(4,0,0,NULL); /* Cancelled late IRQ must not add a token. */
 int value;assert(!sem_getvalue(&completion[0].signal,&value) && value==0);

 reset(0);mode=NEVER;struct audio_playback playback={.pcm=s};
 struct call c={.s=&playback.pcm};pthread_t worker;begin=now_us();
 assert(!pthread_create(&worker,NULL,call_transfer,&c));delay_us(10000);audio_playback_stop(&playback);
 pthread_join(worker,NULL);assert(c.result==-ECANCELED && now_us()-begin<150000);
 assert(atomic_load(&cancel_count)==1);
 reset(1);mode=NEVER;struct audio_capture capture={.pcm=s};capture.pcm.direction=1;
 c=(struct call){.s=&capture.pcm};begin=now_us();
 assert(!pthread_create(&worker,NULL,call_transfer,&c));delay_us(10000);assert(!audio_capture_abort(&capture));
 pthread_join(worker,NULL);assert(c.result==-ECANCELED && now_us()-begin<150000);
 assert(!pthread_once(&completion_once,completion_init) && atomic_load(&init_count)==2);
 puts("PASS actual BES IRQ transfer: no tick poll, immediate completion, EINTR same monotonic deadline, stale signal, late IRQ, start/completion errors, timeout, playback/capture cancellation, permanent semaphores");
 return 0;
}
'''
if __name__=='__main__':main()

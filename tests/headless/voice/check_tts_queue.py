#!/usr/bin/env python3
"""Compile actual bounded PCM queue with explicit slow/error DMA substitutes."""
from pathlib import Path
import argparse, subprocess, tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk',required=True,type=Path)
    sdk=parser.parse_args().sdk
    text=(sdk/'src/voice/voice_channel.c').read_text()
    start=text.index('static struct timespec s_tts_start;')
    end=text.index('/* ── Public API ',start)
    with tempfile.TemporaryDirectory(prefix='zhaoxi-pcm-queue-') as folder:
        p=Path(folder);source=p/'test_queue.c';exe=p/'test_queue'
        source.write_text(PRELUDE+text[start:end]+TESTS)
        for native in (False,True):
            flags=['-D__NuttX__=1'] if native else []
            subprocess.run(['gcc',*flags,'-std=gnu11','-O1','-g','-pthread','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=15)

PRELUDE=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#define CONFIG_ZHAOXI_BES_AUDIO 1
#define AGENT_VOICE_STACK 131072
#define AGENT_TTS_WS_SAMPLE_RATE 24000
#define AGENT_VOICE_CHANNELS 1
#define AGENT_VOICE_BITS 16
typedef struct {int marker;} audio_playback_t;
static struct {pthread_mutex_t lock;int tts_abort;} s_voice={.lock=PTHREAD_MUTEX_INITIALIZER};
static struct {pthread_mutex_t lock;bool tts_playing;} s_auto={.lock=PTHREAD_MUTEX_INITIALIZER};
static const char *TAG="fixture";
static int audio_playback_write(audio_playback_t *,const void *,size_t);
static int fail_malloc,fail_thread;
static void *fixture_malloc(size_t);
static int fixture_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
#define malloc fixture_malloc
#define pthread_create fixture_pthread_create
#ifdef __NuttX__
static int fail_priority,priority_calls;
static int fixture_inherit(pthread_attr_t *a,int value) {(void)a;assert(value==PTHREAD_EXPLICIT_SCHED);priority_calls++;return fail_priority?EPERM:0;}
static int fixture_policy(pthread_attr_t *a,int value) {(void)a;assert(value==SCHED_RR);priority_calls++;return 0;}
static int fixture_priority(pthread_attr_t *a,const struct sched_param *v) {(void)a;assert(v->sched_priority==110);priority_calls++;return 0;}
#define pthread_attr_setinheritsched fixture_inherit
#define pthread_attr_setschedpolicy fixture_policy
#define pthread_attr_setschedparam fixture_priority
#endif
'''

TESTS=r'''
#undef malloc
#undef pthread_create
static void *fixture_malloc(size_t size) {return fail_malloc?NULL:malloc(size);}
static int fixture_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p)
{return fail_thread?EAGAIN:pthread_create(t,a,f,p);}
static unsigned char expected[400000],received[400000];
static size_t received_count;
static unsigned sleep_us;
static int fail_at;
static atomic_int write_calls,hold_write;
static audio_playback_t playback;
static int audio_playback_write(audio_playback_t *pb,const void *pcm,size_t length)
{
    assert(pb==&playback && pcm && !(length%2));
    int call=atomic_fetch_add(&write_calls,1)+1;
    while(atomic_load(&hold_write)) {
        if(voice_channel_tts_stream_error())return -ECANCELED;
        usleep(1000);
    }
    if(sleep_us)usleep(sleep_us);
    if(fail_at && call>=fail_at)return -EIO;
    assert(received_count+length<=sizeof(received));
    memcpy(received+received_count,pcm,length);received_count+=length;
    return (int)length;
}
static void reset(void)
{
    (void)s_asr_done_ts;
    s_voice.tts_abort=s_tts_playback_error=s_tts_first_chunk=0;
    s_auto.tts_playing=false;received_count=0;sleep_us=0;fail_at=0;
    atomic_store(&write_calls,0);atomic_store(&hold_write,0);
    clock_gettime(CLOCK_MONOTONIC,&s_tts_start);
    for(size_t i=0;i<sizeof(expected);i++)expected[i]=(unsigned char)((i*13+i/256)%251);
}
struct producer {struct tts_pcm_queue *queue;size_t count;atomic_int done;};
static void *produce(void *opaque)
{
    struct producer *p=opaque;
    for(size_t offset=0;offset<p->count && !voice_channel_tts_stream_error();) {
        size_t count=p->count-offset;if(count>8178)count=8178;
        tts_enqueue_cb(expected+offset,count,0,p->queue);offset+=count;
    }
    atomic_store(&p->done,1);return NULL;
}
static void wait_writes(int n)
{
    for(int i=0;i<2000 && atomic_load(&write_calls)<n;i++)usleep(1000);
    assert(atomic_load(&write_calls)>=n);
}
int main(void)
{
    struct tts_pcm_queue q;pthread_t player,producer;
    reset();fail_malloc=1;assert(tts_queue_start(&q,&playback,&player)==-ENOMEM);fail_malloc=0;
    fail_thread=1;assert(tts_queue_start(&q,&playback,&player)==-EAGAIN);fail_thread=0;
#ifdef __NuttX__
    fail_priority=1;assert(tts_queue_start(&q,&playback,&player)==-EPERM);fail_priority=0;
    assert(priority_calls>=4);
#endif
    reset();sleep_us=50000;
    assert(tts_queue_start(&q,&playback,&player)==0);
    int64_t begin=tts_queue_now();
    tts_enqueue_cb(expected,12800,0,&q);
    assert(tts_queue_now()-begin<40); /* HTTPS callback doesn't wait 4x50ms DMA. */
    assert(tts_queue_finish(&q,player,0)==0);
    assert(tts_queue_now()-begin>=150 && received_count==12800);
    assert(!memcmp(expected,received,received_count));

    reset();sleep_us=3000;
    assert(tts_queue_start(&q,&playback,&player)==0);
    struct producer p={.queue=&q,.count=300000};
    assert(pthread_create(&producer,NULL,produce,&p)==0);pthread_join(producer,NULL);
    assert(tts_queue_finish(&q,player,0)==0);
    assert(q.peak<=TTS_QUEUE_BYTES && q.peak==TTS_QUEUE_BYTES);
    assert(received_count==p.count && !memcmp(expected,received,p.count));
    assert(q.received_bytes==p.count && q.played_bytes==p.count);

    /* A remainder of 1280 bytes waits briefly for the next 1920 bytes,
     * instead of forcing an extra short DMA transfer on each SSE event. */
    reset();sleep_us=1000;
    assert(tts_queue_start(&q,&playback,&player)==0);
    tts_enqueue_cb(expected,12800,0,&q);wait_writes(4);usleep(10000);
    tts_enqueue_cb(expected+12800,1280,0,&q);usleep(20000);
    assert(atomic_load(&write_calls)==4);
    tts_enqueue_cb(expected+14080,1920,0,&q);wait_writes(5);
    assert(!tts_queue_finish(&q,player,0));
    assert(received_count==16000 && !memcmp(expected,received,received_count));
    assert(q.received_bytes==16000 && q.played_bytes==16000 && q.chunks==3);
    assert(q.min_chunk==1280 && q.max_chunk==12800 && q.fragment_wait_ms>=10);
    assert(q.underflows>=1 && q.gap_ms>=10);

    /* EOF flushes a final short block without waiting 80ms or losing samples. */
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    tts_enqueue_cb(expected,12800,0,&q);wait_writes(4);
    tts_enqueue_cb(expected+12800,640,1,&q);
    begin=tts_queue_now();assert(!tts_queue_finish(&q,player,0));
    assert(tts_queue_now()-begin<60 && received_count==13440);
    assert(!memcmp(expected,received,received_count));

    /* A server that delays DONE after its last audio causes no false
     * underrun: there is no subsequent PCM to be separated by a gap. */
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    tts_enqueue_cb(expected,3200,0,&q);wait_writes(1);usleep(200000);
    assert(!tts_queue_finish(&q,player,0));
    assert(q.underflows==0 && q.empty_ms==0 && q.gap_ms==0);

    /* Less than the target may not wait forever: first audio starts at the
     * measured 640ms cushion deadline, independent of server EOF. */
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    begin=tts_queue_now();tts_enqueue_cb(expected,3200,0,&q);wait_writes(1);
    assert(tts_queue_now()-begin>=TTS_QUEUE_PREFILL_MS-60);
    assert(tts_queue_now()-begin<TTS_QUEUE_PREFILL_MS+300);
    assert(!tts_queue_finish(&q,player,0) && received_count==3200);

    /* Enough PCM starts immediately; a short EOF never waits the cushion. */
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    begin=tts_queue_now();tts_enqueue_cb(expected,TTS_QUEUE_PREFILL,0,&q);wait_writes(1);
    assert(tts_queue_now()-begin<300);
    assert(!tts_queue_finish(&q,player,0) && received_count==TTS_QUEUE_PREFILL);
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    begin=tts_queue_now();tts_enqueue_cb(expected,640,1,&q);
    assert(!tts_queue_finish(&q,player,0) && received_count==640);
    assert(tts_queue_now()-begin<150 && !memcmp(expected,received,640));

    /* Cancellation during initial buffering wakes immediately and plays no
     * queued prefix; error propagation is not delayed by the new cushion. */
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    tts_enqueue_cb(expected,640,0,&q);begin=tts_queue_now();
    assert(tts_queue_finish(&q,player,-ECANCELED)==-ECANCELED);
    assert(tts_queue_now()-begin<150 && !s_tts_first_chunk && !received_count);

    reset();fail_at=2;
    assert(tts_queue_start(&q,&playback,&player)==0);
    p=(struct producer){.queue=&q,.count=300000};
    assert(pthread_create(&producer,NULL,produce,&p)==0);pthread_join(producer,NULL);
    assert(tts_queue_finish(&q,player,0)==-EIO);
    assert(voice_channel_tts_stream_error()==-EIO && received_count==3200);

    reset();atomic_store(&hold_write,1);
    assert(tts_queue_start(&q,&playback,&player)==0);
    p=(struct producer){.queue=&q,.count=300000};
    assert(pthread_create(&producer,NULL,produce,&p)==0);wait_writes(1);
    pthread_mutex_lock(&s_voice.lock);s_voice.tts_abort=1;pthread_mutex_unlock(&s_voice.lock);
    pthread_join(producer,NULL);assert(atomic_load(&p.done));
    assert(tts_queue_finish(&q,player,0)==-ECANCELED);
    assert(received_count==0);

    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    tts_enqueue_cb(expected,640,0,&q);
    assert(tts_queue_finish(&q,player,-ECONNRESET)==-ECONNRESET);

    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    assert(tts_queue_finish(&q,player,0)==0 && !s_tts_first_chunk);
    reset();assert(tts_queue_start(&q,&playback,&player)==0);
    tts_enqueue_cb(expected,3,0,&q);
    assert(tts_queue_finish(&q,player,0)==-EINVAL && !received_count);
    puts("PASS actual PCM queue: network callback decoupled, exact samples, bounded backpressure, prebuffer deadline, DMA error, blocked cancellation, network failure, empty stream and odd samples");
    return 0;
}
'''

if __name__=='__main__':main()

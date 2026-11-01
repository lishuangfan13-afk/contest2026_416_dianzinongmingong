/* SPDX-License-Identifier: Apache-2.0 */
/* Actual voice_channel.c with a strict open -> start -> read capture double.
 * This verifies ordering and failure cleanup, not real audio or cloud ASR. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void *test_malloc(size_t size);
static int test_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
#define malloc test_malloc
#define pthread_create test_pthread_create
#include VOICE_CHANNEL_SOURCE
#undef malloc
#undef pthread_create

struct audio_capture { atomic_int phase; };
struct voice_asr_stream { int marker; };
static struct audio_capture capture;
static struct voice_asr_stream asr_stream;
static atomic_int reads;
static int opens, starts, closes, premature_reads, aborts;
static int stream_mode, fail_start, fail_allocation, fail_thread;
static int stream_finishes, stream_aborts;
static atomic_int fake_input_pattern, fake_asr_calls, fake_bus_messages, fake_shutdown;
static int allow_bus, fail_asr;
static atomic_int block_asr, asr_release;
static atomic_int fake_followup_begin;
static char last_chat_id[32];
static char last_content[512];
static const char *fake_batch_text="fixture_batch";
static pthread_t auto_test_thread;

static void reset_fixture(void)
{
    assert(s_voice.state == VOICE_IDLE && !s_voice.cap && !s_voice.pcm_buf);
    atomic_store(&capture.phase, 0); atomic_store(&reads, 0);
    opens=starts=closes=premature_reads=aborts=0;
    stream_mode=fail_start=fail_allocation=fail_thread=0;
    stream_finishes=stream_aborts=0;
    atomic_store(&fake_input_pattern,0);atomic_store(&fake_asr_calls,0);
    atomic_store(&fake_bus_messages,0);atomic_store(&fake_shutdown,0);
    allow_bus=fail_asr=0;
    atomic_store(&block_asr,0);atomic_store(&asr_release,0);last_chat_id[0]=0;
    atomic_store(&fake_followup_begin,-1);
    last_content[0]=0;fake_batch_text="fixture_batch";
#ifdef CONFIG_ZHAOXI_BES_AUDIO
    assert(voice_channel_auto_wake_set_enabled(false)==0);
#endif
}
static void *test_malloc(size_t size)
{
    if (fail_allocation && size == AGENT_VOICE_PCM_BUF_SIZE) return NULL;
    return malloc(size);
}
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*function)(void *), void *arg)
{
    if (fail_thread) return EAGAIN;
    return pthread_create(thread, attr, function, arg);
}
audio_capture_t *audio_capture_open(const char *path, unsigned rate,
                                   unsigned channels, unsigned bits)
{
    (void)path; assert(rate == 16000 && channels == 1 && bits == 16);
    assert(atomic_load(&capture.phase) == 0);
    opens++; atomic_store(&capture.phase, 2); return &capture;
}
int audio_capture_start(audio_capture_t *cap)
{
    assert(cap == &capture && atomic_load(&capture.phase) == 2);
    starts++;
    /* Old startup ordering deterministically lets the consumer read here. */
    usleep(20000);
    if (fail_start) return -EIO;
    atomic_store(&capture.phase, 3); return 0;
}
int audio_capture_read(audio_capture_t *cap, void *out, size_t length)
{
    assert(cap == &capture);
    int phase=atomic_load(&capture.phase);
    if (phase == 2) { premature_reads++; return -EINVAL; }
    if (phase != 3) return -EINTR;
    usleep(1000);
    if (atomic_load(&capture.phase) != 3) return -EINTR;
    int frame=atomic_fetch_add(&reads,1),pattern=atomic_load(&fake_input_pattern);
    if(pattern) {
        int level=20;
        if(frame>=80 && (pattern==2 ? frame<360 : pattern==4 ? frame<130 : frame<95))level=2000;
        int followup=atomic_load(&fake_followup_begin);
        if(pattern==3 && followup>=0 && frame>=followup && frame<followup+30)level=2000;
        int16_t *samples=out;
        for(size_t i=0;i<length/2;i++)samples[i]=(i&1)?level:-level;
    } else memset(out,1,length);
    return length;
}
int audio_capture_abort(audio_capture_t *cap)
{
    assert(cap == &capture); aborts++;
    atomic_store(&capture.phase, 4); return 0;
}
void audio_capture_close(audio_capture_t *cap)
{
    assert(cap == &capture && atomic_load(&capture.phase) != 0);
    closes++; atomic_store(&capture.phase, 0);
}
void audio_playback_stop(audio_playback_t *pb) { (void)pb; }
voice_asr_stream_t *voice_asr_stream_open(void)
{ return stream_mode ? &asr_stream : NULL; }
int voice_asr_stream_send(voice_asr_stream_t *s, const unsigned char *pcm, size_t size)
{ assert(s == &asr_stream && pcm && size); return 0; }
void voice_asr_stream_abort(voice_asr_stream_t *s)
{ assert(s == &asr_stream); stream_aborts++; }
int voice_asr_stream_finish(voice_asr_stream_t *s, char *out, size_t size)
{ assert(s == &asr_stream); stream_finishes++; snprintf(out,size,"fixture_stream"); return 0; }
int voice_asr_recognize(const unsigned char *pcm, size_t size, char *out, size_t cap)
{
  assert(pcm && size);atomic_fetch_add(&fake_asr_calls,1);
  while(atomic_load(&block_asr) && !atomic_load(&asr_release))usleep(1000);
  if(fail_asr)return -EIO;
  snprintf(out,cap,"%s",fake_batch_text); return 0;
}
int volc_asr_register(void) { return 0; }
int volc_tts_register(void) { return 0; }
int claw_config_get(const char *key,char *out,size_t size)
{ (void)key;if(size)out[0]=0;return -1; }
int message_bus_push_inbound(const agent_msg_t *msg)
{
  if(!allow_bus)abort();
  assert(!strcmp(msg->channel,"voice") && !strncmp(msg->chat_id,"voice_auto_",11));
  assert(strlen(msg->chat_id)<sizeof(last_chat_id));
  memcpy(last_chat_id,msg->chat_id,strlen(msg->chat_id)+1);
  snprintf(last_content,sizeof(last_content),"%s",msg->content);
  atomic_fetch_add(&fake_bus_messages,1);free(msg->content);return 0;
}
bool agent_shutdown_requested(void) { return atomic_load(&fake_shutdown)!=0; }
int agent_task_create(void *(*function)(void *),const char *name,int stack,void *arg,int prio)
{
  (void)name;(void)stack;(void)prio;
  return pthread_create(&auto_test_thread,NULL,function,arg);
}

static void record_and_stop(int streaming)
{
    reset_fixture();stream_mode=streaming;
    assert(voice_channel_start() == 0);
    assert(starts == 1 && atomic_load(&capture.phase) == 3);
    for(int i=0;i<1000 && atomic_load(&reads)<3;i++)usleep(1000);
    assert(atomic_load(&reads)>=3 && premature_reads==0);
    char text[80];assert(voice_channel_stop_with_text(text,sizeof(text))==0);
    assert(!strcmp(text,streaming?"fixture_stream":"fixture_batch"));
    assert(opens==1 && closes==1 && starts==1 && !s_voice.cap);
    assert(streaming ? stream_finishes==1 : stream_finishes==0);
}
int main(void)
{
    assert(voice_channel_init()==0);
    record_and_stop(0);
    record_and_stop(1);
    reset_fixture();stream_mode=1;fail_start=1;
    assert(voice_channel_start()==-EIO);
    assert(opens==1 && starts==1 && closes==1 && atomic_load(&reads)==0);
    assert(stream_aborts==1 && s_voice.state==VOICE_IDLE && !s_voice.cap);
    reset_fixture();fail_allocation=1;
    assert(voice_channel_start()==-ENOMEM);
    assert(opens==1 && starts==0 && closes==1 && atomic_load(&reads)==0);
    assert(s_voice.state==VOICE_IDLE && !s_voice.cap);
    reset_fixture();stream_mode=1;fail_thread=1;
    assert(voice_channel_start()==-EAGAIN);
    assert(opens==1 && starts==0 && closes==1 && stream_aborts==1);
    assert(s_voice.state==VOICE_IDLE && !s_voice.cap);
    puts("capture lifecycle passed: batch and streaming start-before-read, real start failure, allocation failure, thread failure, stop cleanup");
    return 0;
}

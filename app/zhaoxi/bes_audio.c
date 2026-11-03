/* SPDX-License-Identifier: Apache-2.0
 * BES1700 AP direct PCM adapter, using the HAL ABI verified by P27/P30.
 * It deliberately bypasses audio_fake and the absent media service.
 */
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include "voice/voice_channel.h"
#ifdef CONFIG_ZHAOXI_MIMO_VOICE
#include "local_prompts.h"
#include "agent_compat.h"
#endif
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

extern int codec_int_open(void), codec_int_close(void);
extern int codec_int_stream_open(unsigned), codec_int_stream_close(unsigned);
extern int codec_int_stream_setup(unsigned, const void *);
extern int codec_int_stream_start(unsigned), codec_int_stream_stop(unsigned);
extern void hal_codec_dac_mute(int);
extern int codec_int_stream_set_chan_vol(unsigned, unsigned, unsigned);
extern int hal_codec_get_dac_volume(unsigned);
extern unsigned analog_codec_get_dac_dev(void);
extern uint8_t hal_dma_get_chan(unsigned, unsigned);
extern int hal_dma_start(const void *), hal_dma_chan_busy(uint8_t);
extern unsigned hal_dma_cancel(uint8_t);
extern void hal_dma_free_chan(uint8_t);
extern void hal_dma_sync_for_cpu(const void *, unsigned);
extern void hal_dma_sync_for_device(const void *, unsigned);

/* Exact layout used by the best1700 prebuilt HAL in the verified BSP. */
struct codec_config {
    uint32_t bits, rate, channels, map;
    uint8_t dma_volume, path, pad[2];
    uint32_t flags;
};
struct dma_config {
    uint8_t channel, burst;
    uint16_t count;
    uint8_t src_width, dst_width, src_burst, dst_burst, flow, src_periph, dst_periph, pad;
    uint32_t src, dst, handler, start;
};
_Static_assert(sizeof(struct codec_config) == 24, "BES codec ABI");
_Static_assert(sizeof(struct dma_config) == 28, "BES DMA ABI");

struct completion {
    sem_t signal;
    atomic_bool armed, done;
    atomic_uint remain, error;
};
static struct completion completion[2];
static pthread_once_t completion_once = PTHREAD_ONCE_INIT;
static int completion_init_error;
static pthread_mutex_t owner_lock = PTHREAD_MUTEX_INITIALIZER;
static int owner = -1;

/* These counting semaphores outlive all sessions: an IRQ never targets a
 * destroyed object. NuttX sem_post supports ISR signaling with PRIO_NONE. */
static void completion_init(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (sem_init(&completion[i].signal, 0, 0)) {
            completion_init_error = -errno; return;
        }
#ifdef SEM_PRIO_NONE
        if (sem_setprotocol(&completion[i].signal, SEM_PRIO_NONE)) {
            completion_init_error = -errno; return;
        }
#endif
    }
}

struct pcm_session {
    struct dma_config dma;
    void *allocation;
    int16_t *buffer;
    atomic_bool aborted;
    unsigned direction, phase, rate;
    uint8_t channel;
};
struct audio_capture { struct pcm_session pcm; float previous, filtered; };
struct audio_playback {
    struct pcm_session pcm;
    size_t bytes;
    unsigned first_ms,last_ms,dma_ms,gap_ms,max_gap_ms;
    bool measured;
};

static unsigned milliseconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)t.tv_sec * 1000u + (unsigned)t.tv_nsec / 1000000u;
}

static void capture_done(uint8_t ch, unsigned remain, unsigned error, void *next)
{
    (void)ch; (void)next;
    if (!atomic_exchange(&completion[1].armed, false)) return;
    completion[1].remain = remain; completion[1].error = error;
    atomic_store(&completion[1].done, true);
    sem_post(&completion[1].signal);
}
static void playback_done(uint8_t ch, unsigned remain, unsigned error, void *next)
{
    (void)ch; (void)next;
    if (!atomic_exchange(&completion[0].armed, false)) return;
    completion[0].remain = remain; completion[0].error = error;
    atomic_store(&completion[0].done, true);
    sem_post(&completion[0].signal);
}

static void close_session(struct pcm_session *s)
{
    if (!s) return;
    atomic_store(&completion[s->direction].armed, false);
    if (s->direction == 0 && s->phase >= 2) hal_codec_dac_mute(1);
    if (s->channel != 255) hal_dma_cancel(s->channel);
    if (s->phase >= 3) codec_int_stream_stop(s->direction);
    if (s->phase >= 2) codec_int_stream_close(s->direction);
    if (s->phase >= 1) codec_int_close();
    if (s->channel != 255) hal_dma_free_chan(s->channel);
    free(s->allocation);
    s->allocation = NULL; s->channel = 255; s->phase = 0;
    pthread_mutex_lock(&owner_lock);
    if (owner == (int)s->direction) owner = -1;
    pthread_mutex_unlock(&owner_lock);
}

static int open_session(struct pcm_session *s, unsigned direction, unsigned rate)
{
    int initialized = pthread_once(&completion_once, completion_init);
    if (initialized || completion_init_error)
        return initialized ? -initialized : completion_init_error;
    memset(s, 0, sizeof(*s));
    s->direction = direction; s->channel = 255; s->rate = rate;
    pthread_mutex_lock(&owner_lock);
    if (owner != -1) { pthread_mutex_unlock(&owner_lock); return -EBUSY; }
    owner = (int)direction;
    pthread_mutex_unlock(&owner_lock);
    s->allocation = malloc(3200 + 64);
    if (!s->allocation) { close_session(s); return -ENOMEM; }
    s->buffer = (int16_t *)(((uintptr_t)s->allocation + 63) & ~(uintptr_t)63);
    memset(s->buffer, 0, 3200);
    int result = codec_int_open();
    if (result) goto failed;
    s->phase = 1;
    result = codec_int_stream_open(direction);
    if (result) goto failed;
    s->phase = 2;
    struct codec_config config = {16, rate, 1, direction ? 4096 : 1,
                                  direction ? 33 : 3, direction ? 1 : 0, {0, 0}, 0};
    result = codec_int_stream_setup(direction, &config);
    if (result) goto failed;
    if (!direction) {
        hal_codec_dac_mute(1);
    }
    s->channel = hal_dma_get_chan(direction ? 70 : 71, 0);
    if (s->channel == 255) { result = -EBUSY; goto failed; }
    s->dma = (struct dma_config){s->channel, 0, 0, 1, 1, 1, 1,
                                direction ? 2 : 1, direction ? 70 : 0,
                                direction ? 0 : 71, 0, 0, 0,
                                (uint32_t)(uintptr_t)(direction ? capture_done : playback_done), 0};
    printf("BESPCM open direction=%u rate=%u dma=%u real_hal=1\n", direction, rate, s->channel);
    return 0;
failed:
    close_session(s);
    return result < 0 ? result : -EIO;
}

static int start_session(struct pcm_session *s)
{
    if (s->phase == 3) return 0;
    if (s->phase != 2) return -EINVAL;
    int result = codec_int_stream_start(s->direction);
    if (result) return -EIO;
    s->phase = 3;
    if (s->direction) {
        usleep(100000);
        volatile uint32_t *codec = (volatile uint32_t *)0x50300000;
        codec[1] |= 3; usleep(1000); codec[1] &= ~3u;
    } else {
        /* BSP volume 16 is approximately -36 dB, not unity gain.
         * Find 0 dB through the public volume-table API, then apply it after
         * start just as in the audible P30 diagnostic. */
        unsigned level;
        for (level = 0; level <= 127; level++) {
            if (hal_codec_get_dac_volume(level) == 0) break;
        }
        if (level > 127) return -ENOTSUP;
        hal_codec_dac_mute(0);
        result = codec_int_stream_set_chan_vol(0, 1, level);
        if (result) return -EIO;
        unsigned gain = *(volatile unsigned *)0x503000b4 & 0xfffff;
        printf("BESPCM playback volume_index=%u gain_code=%u expected=16384 pcm_divisor=1\n", level, gain);
        if (gain != 16384) return -EIO;
    }
    return 0;
}

static int transfer(struct pcm_session *s, unsigned samples)
{
    if (atomic_load(&s->aborted)) return -ECANCELED;
    s->dma.count = samples;
    if (s->direction) s->dma.dst = (uint32_t)(uintptr_t)s->buffer;
    else s->dma.src = (uint32_t)(uintptr_t)s->buffer;
    struct completion *done = &completion[s->direction];
    while (sem_trywait(&done->signal) == 0) {}
    if (errno != EAGAIN && errno != EINTR) return -errno;
    done->remain = done->error = ~0u; atomic_store(&done->done, false);
    hal_dma_sync_for_device(s->buffer, samples * 2);
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 750000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    atomic_store(&done->armed, true);
    if (hal_dma_start(&s->dma)) { atomic_store(&done->armed, false); return -EIO; }
    while (!atomic_load(&done->done)) {
        int error = 0;
        if (atomic_load(&s->aborted)) error = -ECANCELED;
        else if (sem_clockwait(&done->signal, CLOCK_MONOTONIC, &deadline) && errno != EINTR)
            error = -errno;
        if (error && !atomic_load(&done->done)) {
            atomic_store(&done->armed, false);
            hal_dma_cancel(s->channel); return error;
        }
    }
    if (atomic_load(&s->aborted)) { hal_dma_cancel(s->channel); return -ECANCELED; }
    if (done->remain || done->error || hal_dma_chan_busy(s->channel)) return -EIO;
    if (s->direction) hal_dma_sync_for_cpu(s->buffer, samples * 2);
    return 0;
}

audio_capture_t *audio_capture_open(const char *path, unsigned rate, unsigned channels, unsigned bits)
{
    (void)path;
    if (rate != 16000 || channels != 1 || bits != 16) { errno = ENOTSUP; return NULL; }
    audio_capture_t *cap = calloc(1, sizeof(*cap));
    if (!cap) return NULL;
    int result = open_session(&cap->pcm, 1, rate);
    if (result) { free(cap); errno = -result; return NULL; }
    return cap;
}
int audio_capture_start(audio_capture_t *cap) { return cap ? start_session(&cap->pcm) : -EINVAL; }
int audio_capture_read(audio_capture_t *cap, void *buffer, size_t length)
{
    if (!cap || !buffer || length < 2 || length % 2 || cap->pcm.phase != 3) return -EINVAL;
    unsigned samples = length / 2; if (samples > 1600) samples = 1600;
    int result = transfer(&cap->pcm, samples);
    if (result) return result;
    int16_t *out = buffer;
    for (unsigned i = 0; i < samples; i++) {
        float input = cap->pcm.buffer[i];
        cap->filtered = input - cap->previous + 0.96f * cap->filtered;
        cap->previous = input;
        float value = cap->filtered * 100.0f;
        if (value > 30000) value = 30000;
        if (value < -30000) value = -30000;
        out[i] = (int16_t)value;
    }
    return samples * 2;
}
int audio_capture_abort(audio_capture_t *cap)
{
    if (!cap) return -EINVAL;
    atomic_store(&cap->pcm.aborted, true);
    sem_post(&completion[1].signal);
    return 0;
}
void audio_capture_close(audio_capture_t *cap) { if (cap) { close_session(&cap->pcm); free(cap); } }

audio_playback_t *audio_playback_open(const char *path, unsigned rate, unsigned channels, unsigned bits)
{
    (void)path;
    if ((rate != 16000 && rate != 24000 && rate != 48000) || channels != 1 || bits != 16 || analog_codec_get_dac_dev() != 4) {
        errno = ENOTSUP; return NULL;
    }
    audio_playback_t *pb = calloc(1, sizeof(*pb));
    if (!pb) return NULL;
    int result = open_session(&pb->pcm, 0, rate);
    if (result) { free(pb); errno = -result; return NULL; }
    return pb;
}
int audio_playback_write(audio_playback_t *pb, const void *buffer, size_t length)
{
    if (!pb || !buffer || length % 2) return -EINVAL;
    int result = start_session(&pb->pcm); if (result) return result;
    const int16_t *in = buffer;
    size_t used = 0;
    while (used < length / 2) {
        unsigned samples = length / 2 - used; if (samples > 1600) samples = 1600;
        /* User-selected speech level: preserve the provider's full PCM range.
         * The hardware DAC remains at unity gain; no digital overdrive. */
        for (unsigned i = 0; i < samples; i++) pb->pcm.buffer[i] = in[used+i];
        unsigned begin=milliseconds();
        result = transfer(&pb->pcm, samples); if (result) return result;
        if(!pb->measured){pb->first_ms=begin;pb->measured=true;}
        else {
            unsigned gap=begin-pb->last_ms;
            pb->gap_ms+=gap;if(gap>pb->max_gap_ms)pb->max_gap_ms=gap;
        }
        pb->last_ms=milliseconds();pb->dma_ms+=pb->last_ms-begin;
        pb->bytes+=samples*2u;
        used += samples;
    }
    return (int)length;
}
void audio_playback_stop(audio_playback_t *pb)
{
    if (pb) {
        atomic_store(&pb->pcm.aborted, true);
        sem_post(&completion[0].signal);
    }
}
void audio_playback_close(audio_playback_t *pb)
{
    if(pb) {
        close_session(&pb->pcm);
        printf("BESPCM timing bytes=%zu rate=%u play_ms=%u ideal_ms=%llu dma_ms=%u gap_ms=%u max_gap_ms=%u\n",
            pb->bytes,pb->pcm.rate,pb->measured?pb->last_ms-pb->first_ms:0,
            (unsigned long long)pb->bytes*1000/(pb->pcm.rate*2u),
            pb->dma_ms,pb->gap_ms,pb->max_gap_ms);
        free(pb);
    }
}

#ifdef CONFIG_ZHAOXI_MIMO_VOICE
struct prompt_playback {audio_playback_t *output;};
static int feedback_cancel(void *unused)
{(void)unused;return agent_shutdown_requested();}
static int feedback_pcm(const unsigned char *pcm,size_t length,void *opaque)
{
    struct prompt_playback *p=opaque;
    if(agent_shutdown_requested())return -ECANCELED;
    if(!p->output)p->output=audio_playback_open(NULL,24000,1,16);
    if(!p->output)return -EIO;
    int n=audio_playback_write(p->output,pcm,length);
    return n==(int)length?0:n<0?n:-EIO;
}
#endif
/* Cues are emitted with capture closed and SDK output/auto locks held.
 * Never reenter voice_channel_speak or use a network provider from here. */
void voice_channel_auto_feedback(int event)
{
    static atomic_uint previous = ATOMIC_VAR_INIT(0);
    bool ready = event == VOICE_AUTO_FEEDBACK_READY;
    bool accepted = event == VOICE_AUTO_FEEDBACK_ACCEPTED;
    if (ready) puts("AUTO_VOICE_READY：收音已就绪；先说“你好，夕夕”，对话中可接着提问。");
    else if (accepted) puts("AUTO_VOICE_ACCEPTED：本句话已收取，正在处理；回答结束再说下一句。");
    else {
        unsigned now = milliseconds(), last = atomic_exchange(&previous, now);
        if (event == VOICE_AUTO_FEEDBACK_TOO_LONG)
            puts("AUTO_VOICE_TOO_LONG：本句话超过长度限制，整段未提交；停顿后请分句重说。");
        else printf("AUTO_VOICE_ERROR event=%d：语音处理未完成，正在恢复等待。\n", event);
        if (last && now - last < 2000) return;
    }
#ifdef CONFIG_ZHAOXI_MIMO_VOICE
    /* Only an actual dispatched turn failure gets the longer prompt.
     * ASR errors while asleep retain the short cue, avoiding noise spam. */
    if(event==VOICE_AUTO_FEEDBACK_DISPATCH_ERROR || event==VOICE_AUTO_FEEDBACK_TURN_TIMEOUT) {
        static atomic_uint last_prompt=ATOMIC_VAR_INIT(0);
        unsigned now=milliseconds(),last=atomic_load(&last_prompt);
        if(!last || now-last>=60000) {
            struct prompt_playback output={0};bool handled=false;
            int rc=zhaoxi_prompt_stream("network_error",feedback_pcm,feedback_cancel,&output,&handled);
            if(output.output)audio_playback_close(output.output);
            if(handled){atomic_store(&last_prompt,now);return;}
            if(rc==-ECANCELED)return;
        }
    }
#endif
    audio_playback_t *pb = audio_playback_open(NULL, 16000, 1, 16);
    if (!pb) return;
    static const int16_t wave[16] = {0,3134,5792,7568,8192,7568,5792,3134,
        0,-3134,-5792,-7568,-8192,-7568,-5792,-3134};
    int16_t pcm[1280];
    if (ready || accepted) {
        for (unsigned i = 0; i < 768; i++) pcm[i] = wave[(accepted ? i / 2 : i) % 16];
        audio_playback_write(pb, pcm, 768 * sizeof(pcm[0]));
    } else for (int part = 0; part < 3; part++) {
        for (unsigned i = 0; i < 1280; i++) pcm[i] = part == 1 ? 0 : wave[i % 16];
        if (audio_playback_write(pb, pcm, sizeof(pcm)) != sizeof(pcm)) break;
    }
    audio_playback_close(pb);
}

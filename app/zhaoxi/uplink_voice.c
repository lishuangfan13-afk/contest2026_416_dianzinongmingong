/* SPDX-License-Identifier: Apache-2.0 */
/* Offline PCM/feature IO only. No network ASR and no raw audio persistence. */
#include "uplink_voice.h"
#include "local_prompts.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "core/message_bus.h"
#include "infra/task_lifecycle.h"
#include "voice/audio_capture.h"
#include "voice/audio_playback.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MODEL_DIR AGENT_DATA_DIR "/audio/uplink"
#define MODEL_FILE MODEL_DIR "/commands.bkw"
#define HEADER_BYTES 24u
#define WIRE_MAX (HEADER_BYTES+sizeof(struct uplink_kw_model))
#define FRAME_SAMPLES 320u
#define PRE_FRAMES 10u
#define END_FRAMES 30u
_Static_assert(sizeof(float)==4,"uplink model requires IEEE binary32 floats");
static pthread_mutex_t file_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audio_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t command_lock=PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int cancelled(bool enabled){return enabled && agent_shutdown_requested();}
static uint32_t crc32(const unsigned char *p,size_t n)
{
    uint32_t c=0xffffffffU;
    while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));}
    return ~c;
}
static void put32(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t get32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int native_format(void)
{
    uint32_t one=1,bits=0;float f=1.0f;memcpy(&bits,&f,sizeof(bits));
    return *(unsigned char *)&one==1 && bits==0x3f800000U;
}
static int model_valid(const struct uplink_kw_model *m)
{
    if(!m || m->count[0]>3 || m->count[1]>3 || m->calibrated>1 || m->validated_mask>7 ||
       (!m->calibrated && m->validated_mask))return 0;
    if(!isfinite(m->minimum_margin) || m->minimum_margin<0 || m->minimum_margin>0.75f)return 0;
    for(unsigned c=0;c<2;c++) {
        if(!isfinite(m->radius[c]) || m->radius[c]<0 || m->radius[c]>1000000.0f)return 0;
        for(unsigned i=0;i<m->count[c];i++) {
            const struct uplink_kw_features *f=&m->templates[c][i];
            if(f->frames<8 || f->frames>UPLINK_KW_MAX_FRAMES)return 0;
            for(unsigned j=0;j<f->frames;j++)for(unsigned k=0;k<UPLINK_KW_COEFFS;k++)
                if(!isfinite(f->values[j][k]) || fabsf(f->values[j][k])>10000.0f)return 0;
        }
    }
    if(m->calibrated && (m->count[0]!=3 || m->count[1]!=3 || m->radius[0]<=0 ||
        m->radius[1]<=0 || m->minimum_margin<0.20f))return 0;
    return 1;
}
static int directories(void)
{
    const char *paths[]={AGENT_DATA_DIR,AGENT_DATA_DIR "/audio",MODEL_DIR};
    for(unsigned i=0;i<3;i++) {
        if(mkdir(paths[i],0700) && errno!=EEXIST)return -errno;
        struct stat st;if(stat(paths[i],&st) || !S_ISDIR(st.st_mode))return -ENOTDIR;
    }
    return 0;
}
static int read_bytes(const char *path,unsigned char *out,size_t cap,size_t *length)
{
    struct stat st;if(lstat(path,&st))return -errno;
    if(!S_ISREG(st.st_mode))return -ELOOP;
    if(st.st_size<0 || (uint64_t)st.st_size>cap)return -EFBIG;
    int fd=open(path,O_RDONLY);if(fd<0)return -errno;
    int rc=0;size_t used=0;
    while(used<cap) {
        ssize_t n=read(fd,out+used,cap-used);
        if(n<0 && errno==EINTR)continue;
        if(n<0){rc=-errno;break;}
        if(!n)break;
        used+=(size_t)n;
    }
    unsigned char extra;ssize_t n;
    do {n=read(fd,&extra,1);}while(n<0 && errno==EINTR);
    if(!rc && n)rc=n<0?-errno:-EFBIG;
    if(close(fd) && !rc)rc=-errno;
    if(!rc)*length=used;
    return rc;
}
static int replace_bytes(const char *path,const unsigned char *data,size_t length)
{
    char tmp[256];int n=snprintf(tmp,sizeof(tmp),"%s.new",path);
    if(n<0 || (size_t)n>=sizeof(tmp))return -ENAMETOOLONG;
    int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0 && errno==EEXIST) {
        struct stat st;if(lstat(tmp,&st) || !S_ISREG(st.st_mode))return -ELOOP;
        if(unlink(tmp))return -errno;
        fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
    }
    if(fd<0)return -errno;
    int rc=0;size_t at=0;
    while(at<length) {
        ssize_t count=write(fd,data+at,length-at);
        if(count<0 && errno==EINTR)continue;
        if(count<=0){rc=count<0?-errno:-EIO;break;}
        at+=(size_t)count;
    }
    if(!rc && fsync(fd))rc=-errno;
    if(close(fd) && !rc)rc=-errno;
    unsigned char *verify=malloc(length?length:1);size_t verified=0;
    if(!verify && !rc)rc=-ENOMEM;
    if(!rc)rc=read_bytes(tmp,verify,length,&verified);
    if(!rc && (verified!=length || memcmp(data,verify,length)))rc=-EIO;
    free(verify);
    if(!rc && rename(tmp,path))rc=-errno;
    if(rc)unlink(tmp);
    return rc;
}
int uplink_voice_load(struct uplink_kw_model *out)
{
    if(!out)return -EINVAL;
    memset(out,0,sizeof(*out));if(!native_format())return -ENOTSUP;
    unsigned char *wire=malloc(WIRE_MAX);if(!wire)return -ENOMEM;
    size_t bytes=0;pthread_mutex_lock(&file_lock);
    int rc=read_bytes(MODEL_FILE,wire,WIRE_MAX,&bytes);
    pthread_mutex_unlock(&file_lock);
    if(!rc && (bytes!=WIRE_MAX || memcmp(wire,"BKW1",4) || get32(wire+4)!=1 ||
        get32(wire+8)!=0x01020304U || get32(wire+12)!=sizeof(*out) ||
        get32(wire+16)!=UPLINK_KW_RATE || get32(wire+20)!=crc32(wire+HEADER_BYTES,sizeof(*out))))rc=-EPROTO;
    if(!rc){memcpy(out,wire+HEADER_BYTES,sizeof(*out));if(!model_valid(out))rc=-EPROTO;}
    if(rc)memset(out,0,sizeof(*out));
    free(wire);return rc;
}
int uplink_voice_save(const struct uplink_kw_model *model)
{
    if(!model_valid(model))return -EINVAL;
    if(!native_format())return -ENOTSUP;
    unsigned char *wire=malloc(WIRE_MAX),*old=malloc(WIRE_MAX);
    if(!wire || !old){free(wire);free(old);return -ENOMEM;}
    memcpy(wire,"BKW1",4);put32(wire+4,1);put32(wire+8,0x01020304U);
    put32(wire+12,(uint32_t)sizeof(*model));put32(wire+16,UPLINK_KW_RATE);
    memcpy(wire+HEADER_BYTES,model,sizeof(*model));put32(wire+20,crc32(wire+HEADER_BYTES,sizeof(*model)));
    pthread_mutex_lock(&file_lock);int rc=directories();size_t old_bytes=0;
    if(!rc) {
        int previous=read_bytes(MODEL_FILE,old,WIRE_MAX,&old_bytes);
        if(!previous)rc=replace_bytes(MODEL_FILE ".bak",old,old_bytes);
        else if(previous!=-ENOENT)rc=previous;
    }
    if(!rc)rc=replace_bytes(MODEL_FILE,wire,WIRE_MAX);
    pthread_mutex_unlock(&file_lock);free(old);free(wire);return rc;
}
/* Preload short prompts before opening the DAC. eMMC reads between the
 * blocking one-shot DMA writes can leave audible gaps every 67 ms. */
#define PROMPT_PCM_MAX 480000u
struct prompt_context {bool cancellable;unsigned char *pcm;size_t used,capacity;};
static int prompt_cancel(void *user)
{struct prompt_context *c=user;return cancelled(c->cancellable);}
static int prompt_emit(const unsigned char *data,size_t bytes,void *user)
{
    struct prompt_context *c=user;
    if(prompt_cancel(c))return -ECANCELED;
    if(bytes>c->capacity-c->used)return -EFBIG;
    memcpy(c->pcm+c->used,data,bytes);c->used+=bytes;return 0;
}
int uplink_local_prompt(const char *id,bool cancellable)
{
    if(!id)return -EINVAL;
    if(pthread_mutex_trylock(&audio_lock))return -EBUSY;
    size_t bytes=0;int rc=cancelled(cancellable)?-ECANCELED:zhaoxi_prompt_check(id,&bytes);
    if(!rc && (!bytes || bytes>PROMPT_PCM_MAX))rc=-EFBIG;
    struct prompt_context c={.cancellable=cancellable,.capacity=bytes};bool handled=false;
    if(!rc){c.pcm=malloc(bytes);if(!c.pcm)rc=-ENOMEM;}
    if(!rc)rc=zhaoxi_prompt_stream(id,prompt_emit,prompt_cancel,&c,&handled);
    if(!rc && (!handled || c.used!=bytes))rc=-EIO;
    audio_playback_t *playback=NULL;
    if(!rc && prompt_cancel(&c))rc=-ECANCELED;
    if(!rc){playback=audio_playback_open(NULL,24000,1,16);if(!playback)rc=-(errno?errno:EIO);}
    int64_t started=now_ms();size_t sent=0;
    while(!rc && sent<c.used) {
        if(prompt_cancel(&c)){rc=-ECANCELED;break;}
        size_t count=c.used-sent;if(count>3200)count=3200;
        int n=audio_playback_write(playback,c.pcm+sent,count);
        if(n<0)rc=n;else if((size_t)n!=count)rc=-EIO;else sent+=count;
    }
    if(playback)audio_playback_close(playback);
    printf("UPLINK_PROMPT id=%s bytes=%zu playback_ms=%lld expected_ms=%zu rc=%d\n",
        id,sent,(long long)(now_ms()-started),sent/48,rc);
    free(c.pcm);pthread_mutex_unlock(&audio_lock);return rc;
}
static int frame_level(const int16_t *pcm)
{
    unsigned sum=0;
    for(unsigned i=0;i<FRAME_SAMPLES;i++){int v=pcm[i];sum+=(unsigned)(v<0?-v:v);}
    return (int)(sum/FRAME_SAMPLES);
}
int uplink_voice_capture(struct uplink_kw_features *out,bool cancellable,unsigned listen_seconds)
{
    if(!out || !listen_seconds || listen_seconds>20)return -EINVAL;
    memset(out,0,sizeof(*out));
    if(pthread_mutex_trylock(&audio_lock))return -EBUSY;
    int16_t *pcm=malloc(UPLINK_KW_MAX_SAMPLES*sizeof(*pcm));
    int16_t *pre=calloc(PRE_FRAMES*FRAME_SAMPLES,sizeof(*pre));
    audio_capture_t *cap=NULL;int rc=0;
    if(!pcm || !pre){rc=-ENOMEM;goto done;}
    if(cancelled(cancellable)){rc=-ECANCELED;goto done;}
    cap=audio_capture_open(NULL,UPLINK_KW_RATE,1,16);
    if(!cap){rc=-(errno?errno:EIO);goto done;}
    rc=audio_capture_start(cap);if(rc)goto done;
    int16_t frame[FRAME_SAMPLES];size_t have=0,used=0;
    unsigned settling=5,pre_at=0,pre_count=0,consecutive=0,voiced=0,silent=0;
    int noise=80,threshold=340;bool active=false,completed=false;
    int64_t deadline=now_ms()+(int64_t)listen_seconds*1000;
    while(now_ms()<deadline && !cancelled(cancellable)) {
        int n=audio_capture_read(cap,(unsigned char *)frame+have,sizeof(frame)-have);
        if(n==-EAGAIN || n==-EWOULDBLOCK){usleep(1000);continue;}
        if(n<=0 || (size_t)n>sizeof(frame)-have){rc=n<0?n:-EIO;break;}
        have+=(size_t)n;if(have<sizeof(frame))continue;have=0;
        if(settling) {
            if(!--settling){puts("UPLINK_LISTEN ready=1");fflush(stdout);}
            continue;
        }
        int level=frame_level(frame);bool loud=level>threshold;
        if(!active) {
            memcpy(pre+pre_at*FRAME_SAMPLES,frame,sizeof(frame));pre_at=(pre_at+1)%PRE_FRAMES;
            if(pre_count<PRE_FRAMES)pre_count++;
            if(loud)consecutive++;
            else {consecutive=0;noise=(noise*31+level)/32;threshold=noise*3+100;if(threshold<180)threshold=180;}
            if(consecutive<3)continue;
            active=true;voiced=consecutive;silent=0;
            unsigned first=(pre_at+PRE_FRAMES-pre_count)%PRE_FRAMES;
            for(unsigned i=0;i<pre_count;i++) {
                memcpy(pcm+used,pre+((first+i)%PRE_FRAMES)*FRAME_SAMPLES,sizeof(frame));used+=FRAME_SAMPLES;
            }
        } else {
            if(used+FRAME_SAMPLES>UPLINK_KW_MAX_SAMPLES){rc=-E2BIG;break;}
            memcpy(pcm+used,frame,sizeof(frame));used+=FRAME_SAMPLES;
            if(loud){voiced++;silent=0;}else silent++;
            if(silent>=END_FRAMES){rc=voiced>=12?0:-ENODATA;completed=!rc;break;}
        }
    }
    if(cancelled(cancellable))rc=-ECANCELED;
    else if(!rc && !completed)rc=-ETIMEDOUT;
    /* Release the device before extracting, playing or returning on every path. */
    if(cap){audio_capture_close(cap);cap=NULL;}
    if(!rc)rc=uplink_kw_extract(pcm,used,out);
    printf("UPLINK_CAPTURE rc=%d samples=%zu voiced_ms=%u threshold=%d\n",rc,used,voiced*20,threshold);
done:
    if(cap)audio_capture_close(cap);
    free(pre);free(pcm);pthread_mutex_unlock(&audio_lock);return rc;
}
static int label(const char *s)
{return !strcmp(s,"wired")?1:!strcmp(s,"wifi")?2:!strcmp(s,"unknown")?0:-1;}
int uplink_voice_command(int argc,char **argv)
{
    if(argc<2 || !argv || !argv[1])return -EINVAL;
    const char *op=argv[1];bool status=!strcmp(op,"status");
    bool simple=status || !strcmp(op,"reset") || !strcmp(op,"calibrate") || !strcmp(op,"test");
    bool learn=!strcmp(op,"learn"),validate=!strcmp(op,"validate");
    if((simple && argc!=2) || (!simple && !learn && !validate) || ((learn || validate) && argc!=3))return -EINVAL;
    int expected=learn || validate?label(argv[2]):-1;
    if((learn && expected<1) || (validate && expected<0))return -EINVAL;
    if(!status && (message_bus_is_ready() || agent_instance_is_active())) {
        puts("UPLINK_TRAIN refused=agent_active");return -EBUSY;
    }
    if(!status && pthread_mutex_trylock(&command_lock))return -EBUSY;
    struct uplink_kw_model *m=calloc(1,sizeof(*m));
    if(!m){if(!status)pthread_mutex_unlock(&command_lock);return -ENOMEM;}
    int rc=uplink_voice_load(m);
    if(status) {
        printf("UPLINK_MODEL rc=%d wired=%u wifi=%u calibrated=%u validation=%u ready=%d radius_wired=%.6f radius_wifi=%.6f margin=%.6f\n",
            rc,m->count[0],m->count[1],m->calibrated,m->validated_mask,uplink_kw_ready(m),
            (double)m->radius[0],(double)m->radius[1],(double)m->minimum_margin);
        free(m);return rc==-ENOENT?0:rc;
    }
    if(!strcmp(op,"reset")){memset(m,0,sizeof(*m));rc=uplink_voice_save(m);}
    else {
        if(rc==-ENOENT && learn)rc=0;
        if(!rc && learn && m->count[expected-1]>=UPLINK_KW_PER_CLASS)rc=-ENOSPC;
        if(!rc && !strcmp(op,"test") && !uplink_kw_ready(m))rc=-EAGAIN;
        if(!rc && !strcmp(op,"calibrate"))rc=uplink_kw_calibrate(m);
        else if(!rc && (learn || validate || !strcmp(op,"test"))) {
            struct uplink_kw_features *f=calloc(1,sizeof(*f));
            if(!f)rc=-ENOMEM;
            const char *prompt=!strcmp(op,"test")?"uplink_choose":expected==1?"learn_wired":expected==2?"learn_wifi":"learn_unknown";
            if(!rc)rc=uplink_local_prompt(prompt,false);
            if(!rc)rc=uplink_voice_capture(f,false,20);
            struct uplink_kw_result result={0};
            if(!rc)rc=learn?uplink_kw_add(m,expected,f):validate?uplink_kw_validate(m,expected,f,&result):uplink_kw_classify(m,f,&result);
            if(validate || !strcmp(op,"test"))printf("UPLINK_SCORE expected=%d label=%d wired=%.6f wifi=%.6f margin=%.6f rc=%d\n",
                expected,result.label,(double)result.scores[0],(double)result.scores[1],(double)result.margin,rc);
            free(f);
            /* A failed independent validation revokes persisted readiness too. */
            if(validate && rc) {m->validated_mask=0;int saved=uplink_voice_save(m);if(saved)rc=saved;}
        }
        if(!rc && strcmp(op,"test"))rc=uplink_voice_save(m);
    }
    printf("UPLINK_TRAIN op=%s rc=%d wired=%u wifi=%u validation=%u ready=%d\n",
        op,rc,m->count[0],m->count[1],m->validated_mask,uplink_kw_ready(m));
    free(m);pthread_mutex_unlock(&command_lock);return rc;
}

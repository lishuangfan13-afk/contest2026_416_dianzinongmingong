/* SPDX-License-Identifier: Apache-2.0 */
/* Include the actual IO implementation to inspect synthetic fixed-path files. */
#include "uplink_voice.c"
#include <assert.h>

static int sync_fail,rename_fail;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{if(sync_fail){sync_fail=0;errno=EIO;return -1;}return __real_fsync(fd);}
int __real_rename(const char *from,const char *to);
int __wrap_rename(const char *from,const char *to)
{if(rename_fail && !strcmp(to,MODEL_FILE)){rename_fail=0;errno=EIO;return -1;}return __real_rename(from,to);}
static int64_t fake_ms;
int __wrap_clock_gettime(clockid_t id,struct timespec *out)
{(void)id;out->tv_sec=(time_t)(fake_ms/1000);out->tv_nsec=(long)(fake_ms%1000)*1000000;return 0;}
static bool shutdown_flag,bus_ready,instance_active;
bool agent_shutdown_requested(void){return shutdown_flag;}
bool message_bus_is_ready(void){return bus_ready;}
bool agent_instance_is_active(void){return instance_active;}

struct audio_capture {unsigned samples;};
struct audio_playback {unsigned writes;};
static struct audio_capture capture;
static struct audio_playback playback;
static unsigned opens,closes,reads,play_opens,play_closes;
static int pattern,short_write,read_error,cancel_after,start_error;
static bool prompt_loaded;
int zhaoxi_prompt_check(const char *id,size_t *bytes)
{prompt_loaded=false;if(!strcmp(id,"missing"))return -ENOENT;*bytes=64;return 0;}
audio_capture_t *audio_capture_open(const char *path,unsigned rate,unsigned channels,unsigned bits)
{(void)path;assert(rate==16000 && channels==1 && bits==16);opens++;capture.samples=0;return &capture;}
int audio_capture_start(audio_capture_t *cap){assert(cap==&capture);return start_error;}
int audio_capture_abort(audio_capture_t *cap){(void)cap;return 0;}
void audio_capture_close(audio_capture_t *cap){assert(cap==&capture);closes++;}
int audio_capture_read(audio_capture_t *cap,void *out,size_t bytes)
{
    assert(cap==&capture && bytes==640);reads++;fake_ms+=20;
    if(cancel_after && reads==(unsigned)cancel_after)shutdown_flag=true;
    if(read_error)return read_error;
    unsigned frame=cap->samples/320;
    bool speech=pattern==1?(frame>=25 && frame<65):pattern==2?frame>=25:
        pattern==3?(frame>=25 && frame<30):false;
    int16_t *pcm=out;
    for(unsigned i=0;i<320;i++)pcm[i]=speech?(int16_t)(3000.0*sin((double)(cap->samples+i)*2.0*3.141592653589793*500.0/16000.0)):20;
    cap->samples+=320;return (int)bytes;
}
audio_playback_t *audio_playback_open(const char *path,unsigned rate,unsigned channels,unsigned bits)
{(void)path;assert(prompt_loaded && rate==24000 && channels==1 && bits==16);play_opens++;playback.writes=0;return &playback;}
int audio_playback_write(audio_playback_t *pb,const void *data,size_t bytes)
{assert(pb==&playback && data);pb->writes++;return short_write?(int)bytes-2:(int)bytes;}
void audio_playback_close(audio_playback_t *pb){assert(pb==&playback);play_closes++;}
int zhaoxi_prompt_stream(const char *id,zhaoxi_prompt_emit emit,zhaoxi_prompt_cancel cancel,void *ctx,bool *handled)
{
    *handled=false;if(!strcmp(id,"missing"))return -ENOENT;
    *handled=true;if(cancel(ctx))return -ECANCELED;
    unsigned char pcm[32]={0};int rc=emit(pcm,sizeof(pcm),ctx);
    if(!rc)rc=emit(pcm,sizeof(pcm),ctx);
    prompt_loaded=!rc;return rc;
}
static void file_write(const unsigned char *wire,size_t size)
{
    FILE *f=fopen(MODEL_FILE,"wb");assert(f);
    assert(fwrite(wire,1,size,f)==size && !fclose(f));
}
static unsigned char *file_read(const char *path,size_t *size)
{unsigned char *wire=malloc(WIRE_MAX+1);assert(wire);assert(!read_bytes(path,wire,WIRE_MAX+1,size));return wire;}
static struct uplink_kw_model *example(void)
{
    struct uplink_kw_model *m=calloc(1,sizeof(*m));assert(m);
    m->count[0]=1;m->templates[0][0].frames=8;
    for(unsigned i=0;i<8;i++)for(unsigned j=0;j<13;j++)m->templates[0][0].values[i][j]=(float)i+(float)j/10;
    return m;
}
static void storage_tests(void)
{
    struct uplink_kw_model *m=example(),*readback=calloc(1,sizeof(*m));assert(readback);
    assert(uplink_voice_load(readback)==-ENOENT);
    assert(!uplink_voice_save(m) && !uplink_voice_load(readback) && !memcmp(m,readback,sizeof(*m)));
    size_t bytes=0;unsigned char *wire=file_read(MODEL_FILE,&bytes);assert(bytes==WIRE_MAX);
    m->templates[0][0].values[0][0]+=1;
    assert(!uplink_voice_save(m));size_t backup_bytes=0;
    unsigned char *backup=file_read(MODEL_FILE ".bak",&backup_bytes);
    assert(backup_bytes==bytes && !memcmp(backup,wire,bytes));free(backup);
    struct uplink_kw_model *before=malloc(sizeof(*m));assert(before);memcpy(before,m,sizeof(*m));
    m->count[1]=4;assert(uplink_voice_save(m)==-EINVAL);m->count[1]=0;
    assert(!uplink_voice_load(readback) && !memcmp(readback,before,sizeof(*m)));
    m->templates[0][0].values[0][0]+=1;sync_fail=1;
    assert(uplink_voice_save(m)==-EIO && !uplink_voice_load(readback) && !memcmp(readback,before,sizeof(*m)));
    rename_fail=1;
    assert(uplink_voice_save(m)==-EIO && !uplink_voice_load(readback) && !memcmp(readback,before,sizeof(*m)));
    assert(!symlink("unrelated",MODEL_FILE ".new"));
    assert(uplink_voice_save(m)==-ELOOP && !uplink_voice_load(readback));unlink(MODEL_FILE ".new");

    wire[20]^=1;file_write(wire,bytes);
    assert(uplink_voice_load(readback)==-EPROTO && !readback->count[0]);wire[20]^=1;
    file_write(wire,bytes-1);assert(uplink_voice_load(readback)==-EPROTO);
    /* Even a valid CRC cannot bless non-finite model values or bad counts. */
    struct uplink_kw_model *bad=(struct uplink_kw_model *)(void *)(wire+HEADER_BYTES);
    bad->count[0]=4;put32(wire+20,crc32(wire+HEADER_BYTES,sizeof(*bad)));file_write(wire,bytes);
    assert(uplink_voice_load(readback)==-EPROTO);
    bad->count[0]=1;bad->templates[0][0].values[0][0]=NAN;
    put32(wire+20,crc32(wire+HEADER_BYTES,sizeof(*bad)));file_write(wire,bytes);
    assert(uplink_voice_load(readback)==-EPROTO);
    assert(!uplink_voice_save(before)); /* Corrupt original is retained verbatim as a backup. */
    backup=file_read(MODEL_FILE ".bak",&backup_bytes);
    assert(backup_bytes==bytes && !memcmp(backup,wire,bytes));free(backup);
    assert(!uplink_voice_load(readback));
    file_write(wire,HEADER_BYTES-1);assert(uplink_voice_load(readback)==-EPROTO);
    unsigned char *oversize=calloc(1,WIRE_MAX+1);assert(oversize);file_write(oversize,WIRE_MAX+1);
    assert(uplink_voice_load(readback)==-EFBIG && uplink_voice_save(before)==-EFBIG);free(oversize);
    unlink(MODEL_FILE);assert(!uplink_voice_save(before));
    free(before);free(wire);free(readback);free(m);
}
static void fixture(int type)
{pattern=type;reads=0;shutdown_flag=false;read_error=start_error=cancel_after=0;fake_ms=0;}
static void audio_tests(void)
{
    struct uplink_kw_features *f=malloc(sizeof(*f));assert(f);
    fixture(0);unsigned old=closes;assert(uplink_voice_capture(f,false,1)==-ETIMEDOUT && closes==old+1 && !f->frames);
    fixture(1);shutdown_flag=true;old=opens;
    assert(uplink_voice_capture(f,true,20)==-ECANCELED && opens==old);
    assert(!uplink_voice_capture(f,false,20) && f->frames>=16); /* stopped Agent flag is ignored only in manual capture */
    fixture(2);old=closes;assert(uplink_voice_capture(f,false,20)==-E2BIG && closes==old+1 && !f->frames);
    fixture(3);assert(uplink_voice_capture(f,false,20)==-ENODATA);
    fixture(1);cancel_after=50;old=closes;
    assert(uplink_voice_capture(f,true,20)==-ECANCELED && closes==old+1);
    fixture(1);start_error=-EIO;old=closes;
    assert(uplink_voice_capture(f,false,20)==-EIO && closes==old+1);
    fixture(1);read_error=-EPIPE;old=closes;
    assert(uplink_voice_capture(f,false,20)==-EPIPE && closes==old+1);
    assert(uplink_voice_capture(f,false,21)==-EINVAL);
    fixture(1);assert(!uplink_local_prompt("uplink_choose",false) && play_opens==play_closes);
    short_write=1;assert(uplink_local_prompt("uplink_choose",false)==-EIO && play_opens==play_closes);short_write=0;
    shutdown_flag=true;old=play_opens;
    assert(uplink_local_prompt("uplink_choose",true)==-ECANCELED && play_opens==old);
    assert(uplink_local_prompt("missing",false)==-ENOENT);
    free(f);assert(opens==closes);
}
static void command_tests(void)
{
    char *reset[]={"uplink-voice","reset"};unsigned old=opens;
    instance_active=true;assert(uplink_voice_command(2,reset)==-EBUSY);instance_active=false;
    bus_ready=true;assert(uplink_voice_command(2,reset)==-EBUSY);bus_ready=false;assert(opens==old);
    char *status[]={"uplink-voice","status"};instance_active=true;
    assert(!uplink_voice_command(2,status));instance_active=false;
    assert(!uplink_voice_command(2,reset));
    struct uplink_kw_model *m=malloc(sizeof(*m));assert(m);
    assert(!uplink_voice_load(m) && !m->count[0] && !m->count[1]);free(m);
    char *bad[]={"uplink-voice","learn","unknown"};assert(uplink_voice_command(3,bad)==-EINVAL);
    char *test[]={"uplink-voice","test"};assert(uplink_voice_command(2,test)==-EAGAIN && opens==old);
}
int main(void)
{
    storage_tests();audio_tests();command_tests();
    puts("PASS: actual model CRC/length/semantic validation, backups and failed-write rollback; local-only capture bounds/cancel/short voice; local playback cleanup and stopped-Agent gating");
    return 0;
}

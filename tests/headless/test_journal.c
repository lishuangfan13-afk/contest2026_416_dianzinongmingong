/* SPDX-License-Identifier: Apache-2.0 */
#include "journal.h"
#include "agent_config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>

static int fail_write, partial_calls, fail_sync, fail_read, corrupt_read, wrote, fail_close;
static atomic_bool hold_sync, inside_sync;
ssize_t __real_write(int fd,const void *data,size_t count);
ssize_t __wrap_write(int fd,const void *data,size_t count)
{
    if(fail_write==1||(fail_write==2&&partial_calls++)) {errno=ENOSPC;return -1;}
    if(fail_write==2)count/=2;
    ssize_t n=__real_write(fd,data,count);if(n>0)wrote=1;return n;
}
ssize_t __real_read(int fd,void *data,size_t count);
ssize_t __wrap_read(int fd,void *data,size_t count)
{
    if(fail_read){errno=EIO;return -1;}
    ssize_t n=__real_read(fd,data,count);
    if(corrupt_read&&wrote&&n>0)((char *)data)[0]^=1;
    return n;
}
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if(atomic_load(&hold_sync)){
        atomic_store(&inside_sync,true);
        while(atomic_load(&hold_sync)){struct timespec t={0,1000000};nanosleep(&t,NULL);}
    }
    if(fail_sync){errno=EIO;return -1;}
    return __real_fsync(fd);
}
int __real_close(int fd);
int __wrap_close(int fd)
{int rc=__real_close(fd);if(fail_close){errno=EIO;return -1;}return rc;}
static void put(const char *path,const char *text)
{FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static char *get(const char *path)
{FILE *f=fopen(path,"rb");assert(f);char *data=calloc(1,65538);assert(data);assert(fread(data,1,65537,f)<65537);assert(!fclose(f));return data;}
static struct zhaoxi_note_result run(time_t utc,const char *op,const char *text,char **reply)
{struct zhaoxi_note_result result;assert(!zhaoxi_note_execute(utc,op,text,&result,reply));assert(result.verified&&*reply&&!result.storage_uncertain);return result;}
struct writer {unsigned id;int rc;};
static void *append(void *value)
{
    struct writer *writer=value;char text[64],*reply=NULL;
    snprintf(text,sizeof(text),"并发记录_%u",writer->id);
    struct zhaoxi_note_result result;
    for(unsigned i=0;i<3000;i++) {
        writer->rc=zhaoxi_note_execute(1793548800,"add",text,&result,&reply);
        if(writer->rc!=-EBUSY)break;
        struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    }
    if(!writer->rc){assert(result.verified);free(reply);}return NULL;
}
int main(void)
{
    char path[256];
    const char *zones[] = {"UTC0", "CST-8", "EST5", NULL};
    for (int i = 0; zones[i]; i++) {
        assert(setenv("TZ", zones[i], 1) == 0); tzset();
        /* 2026-11-01 15:59:59 UTC and the very next Beijing midnight. */
        assert(zhaoxi_note_path((time_t)1793548799, path, sizeof(path)) == 0);
        assert(!strcmp(path, AGENT_DATA_DIR "/memory/daily/2026-11-01.md"));
        assert(zhaoxi_note_path((time_t)1793548800, path, sizeof(path)) == 0);
        assert(!strcmp(path, AGENT_DATA_DIR "/memory/daily/2026-11-02.md"));
    }
    assert(zhaoxi_note_path(123, path, sizeof(path)) != 0);
    assert(zhaoxi_note_path((time_t)1793548800, path, 4) != 0);
    assert(zhaoxi_note_path((time_t)1793548800, NULL, 0) != 0);
    const time_t utc=1793548800;
    assert(!zhaoxi_note_path(utc,path,sizeof(path)));
    char *reply=NULL;struct zhaoxi_note_result result;
    result=run(utc,"list",NULL,&reply);assert(!result.bytes&&strstr(reply,"没有备忘"));free(reply);
    result=run(utc,"add","第一条中文 内容",&reply);assert(strstr(reply,"读回确认"));free(reply);
    const char *legacy="# 原有记录\n- 老备忘 不能覆盖\n没有结尾换行";
    put(path,legacy);result=run(utc,"add","新备忘",&reply);free(reply);
    char *data=get(path);assert(!strncmp(data,legacy,strlen(legacy))&&strstr(data,"\n- 新备忘\n"));free(data);
    result=run(utc,"list",NULL,&reply);assert(strstr(reply,legacy)&&strstr(reply,"新备忘"));free(reply);
    assert(zhaoxi_note_execute(123,"add","错误日期",&result,&reply)==-EINVAL&&!reply);
    assert(zhaoxi_note_execute(utc,"add","注入\n新行",&result,&reply)==-EINVAL&&!reply);
    char huge[513];memset(huge,'X',512);huge[512]=0;
    assert(zhaoxi_note_execute(utc,"add",huge,&result,&reply)==-E2BIG&&!reply);
    for(int scenario=0;scenario<6;scenario++) {
        put(path,legacy);wrote=partial_calls=0;
        fail_write=scenario==0?1:scenario==1?2:0;
        fail_sync=scenario==2;fail_read=scenario==3;corrupt_read=scenario==4;fail_close=scenario==5;
        assert(zhaoxi_note_execute(utc,"add","不许虚假确认",&result,&reply)<0&&!reply&&!result.verified);
        assert(!wrote||result.storage_uncertain);
        fail_write=fail_sync=fail_read=corrupt_read=fail_close=0;
        data=get(path);assert(!strncmp(data,legacy,strlen(legacy)));free(data);
        result=run(utc,"list",NULL,&reply);free(reply); /* Failure released local lock. */
    }
    put(path,legacy);
    atomic_store(&hold_sync,true);atomic_store(&inside_sync,false);
    struct writer blocked={100,-1};pthread_t first;
    assert(!pthread_create(&first,NULL,append,&blocked));
    for(unsigned i=0;i<3000&&!atomic_load(&inside_sync);i++){struct timespec t={0,1000000};nanosleep(&t,NULL);}
    assert(atomic_load(&inside_sync));
    assert(zhaoxi_note_execute(utc,"add","并发立即拒绝",&result,&reply)==-EBUSY&&!reply);
    atomic_store(&hold_sync,false);assert(!pthread_join(first,NULL)&&!blocked.rc);
    pthread_t threads[8];struct writer writers[8];
    for(unsigned i=0;i<8;i++){writers[i]=(struct writer){i,-1};assert(!pthread_create(&threads[i],NULL,append,&writers[i]));}
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL)&&!writers[i].rc);
    data=get(path);assert(!strncmp(data,legacy,strlen(legacy)));
    for(unsigned i=0;i<8;i++){char text[64];snprintf(text,sizeof(text),"并发记录_%u\n",i);char *at=strstr(data,text);assert(at&&!strstr(at+strlen(text),text));}free(data);
    char *limit=malloc(65537);assert(limit);memset(limit,'L',65536);limit[65536]=0;put(path,limit);
    assert(zhaoxi_note_execute(utc,"add","已满",&result,&reply)==-EFBIG&&!reply&&!result.storage_uncertain);
    data=get(path);assert(!strcmp(data,limit));free(data);free(limit);
    unlink(path);
    puts("PASS: deterministic local note append/list, verified readback, original-byte preservation, bounded concurrent requests, storage failures and Beijing midnight independent of TZ.");
    return 0;
}

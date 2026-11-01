/* SPDX-License-Identifier: Apache-2.0 */
#include "todo_store.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail_sync, fail_rename, short_write;
int __real_fsync(int fd);
int __wrap_fsync(int fd) { if (fail_sync) { errno=EIO; return -1; } return __real_fsync(fd); }
int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{ if (fail_rename) { errno=EIO; return -1; } return __real_rename(from,to); }
size_t __real_fwrite(const void *p, size_t size, size_t n, FILE *file);
size_t __wrap_fwrite(const void *p, size_t size, size_t n, FILE *file)
{ if (short_write) { errno=ENOSPC; return __real_fwrite(p,size,n/2,file); } return __real_fwrite(p,size,n,file); }

static void write_text(const char *path, const char *text)
{ FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f)); }
static char *read_text(const char *path)
{ FILE *f=fopen(path,"rb");assert(f);char *s=calloc(1,65538);assert(s);assert(fread(s,1,65537,f)<65537);assert(!fclose(f));return s; }
static struct zhaoxi_todo_result run(const char *path, const char *op, const char *arg, char **reply)
{ struct zhaoxi_todo_result r;assert(!zhaoxi_todo_execute(path,op,arg,&r,reply));assert(*reply);return r; }
struct concurrent { const char *path; unsigned id; };
static void *append(void *value)
{ struct concurrent *c=value;char *reply;struct zhaoxi_todo_result r=run(c->path,"add","同文中文任务",&reply);c->id=r.id;free(reply);return NULL; }

int main(void)
{
    char directory[]="/tmp/zhaoxi-todo-XXXXXX";assert(mkdtemp(directory));
    char path[160];snprintf(path,sizeof(path),"%s/TASKS.md",directory);
    char *reply=NULL;struct zhaoxi_todo_result r=run(path,"list",NULL,&reply);
    assert(r.count==0 && strstr(reply,"没有未完成"));free(reply);
    const char *legacy="# 原有待办\n- [ ] [2] 原有任务\n- [X] [7] 已完成\r\n备注【保留】";
    write_text(path,legacy);
    r=run(path,"add","水杯 你好",&reply);assert(r.id==8);free(reply);
    char *first=read_text(path);assert(!strncmp(first,legacy,strlen(legacy)));
    assert(strstr(first,"\n- [ ] [8] 水杯 你好\n"));free(first);
    r=run(path,"add","水杯 你好",&reply);assert(r.id==9);free(reply);
    r=run(path,"list",NULL,&reply);assert(r.count==3);
    assert(strstr(reply,"[2] 原有任务") && strstr(reply,"[8] 水杯 你好") && strstr(reply,"[9] 水杯 你好"));
    assert(!strstr(reply,"已完成"));free(reply);
    char *before=read_text(path);r=run(path,"done","8",&reply);assert(r.id==8 && r.changed==1);free(reply);
    char *mark=strstr(before,"- [ ] [8]");assert(mark);mark[3]='x';
    char *after=read_text(path);assert(!strcmp(before,after));free(after);
    r=run(path,"done","8",&reply);assert(r.changed==0);free(reply);
    after=read_text(path);assert(!strcmp(before,after));free(after);
    assert(zhaoxi_todo_execute(path,"done","777",&r,&reply)==-ENOENT && !reply);
    assert(zhaoxi_todo_execute(path,"done","-1",&r,&reply)==-EINVAL && !reply);
    for (int failure=0;failure<3;failure++) {
        fail_sync=failure==0;fail_rename=failure==1;short_write=failure==2;
        assert(zhaoxi_todo_execute(path,"add","不能假确认",&r,&reply)<0 && !reply);
        after=read_text(path);assert(!strcmp(before,after));free(after);
    }
    fail_sync=fail_rename=short_write=0;free(before);
    r=run(path,"add","编号继续递增",&reply);assert(r.id==10);free(reply);
    write_text(path,"- [ ] [9999] 容量末尾\n");
    assert(zhaoxi_todo_execute(path,"add","不复用",&r,&reply)==-ENOSPC && !reply);
    write_text(path,"- [ ] [2] 一\n- [x] [2] 二\n");
    assert(zhaoxi_todo_execute(path,"add","不覆盖",&r,&reply)==-EINVAL && !reply);
    write_text(path,"- [ ] 无编号旧项\n");
    assert(zhaoxi_todo_execute(path,"add","不覆盖",&r,&reply)==-EINVAL && !reply);
    after=read_text(path);assert(!strcmp(after,"- [ ] 无编号旧项\n"));free(after);
    unlink(path);
    struct concurrent jobs[8];pthread_t threads[8];
    for (unsigned i=0;i<8;i++) { jobs[i]=(struct concurrent){path,0};assert(!pthread_create(&threads[i],NULL,append,&jobs[i])); }
    unsigned seen=0;
    for (unsigned i=0;i<8;i++) { assert(!pthread_join(threads[i],NULL));assert(jobs[i].id>=1&&jobs[i].id<=8);seen|=1u<<jobs[i].id; }
    assert(seen==0x1fe);r=run(path,"list",NULL,&reply);assert(r.count==8);free(reply);
    unlink(path);rmdir(directory);
    puts("PASS: deterministic UTF-8 TODO storage, stable IDs, duplicate text, exact completion, retained history, concurrent adds and failed-write rollback.");
    return 0;
}

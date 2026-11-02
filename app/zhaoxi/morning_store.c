/* SPDX-License-Identifier: Apache-2.0 */
#include "morning.h"
#include "agent_config.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef AGENT_USER_FILE
#define AGENT_USER_FILE AGENT_DATA_DIR "/config/USER.md"
#endif
#define NEWS_DIR AGENT_DATA_DIR "/news"
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;

static int utf8_text(const char *s, size_t max)
{
    size_t len = s ? strlen(s) : 0;
    if (!len || len > max) return -EINVAL;
    for (size_t i = 0; i < len;) {
        unsigned c = (unsigned char)s[i++], n, v, min;
        if (c < 128) { if (c < 32 && c != '\n' && c != '\t') return -EINVAL; continue; }
        if ((c & 0xe0) == 0xc0) {n=1;v=c&31;min=128;}
        else if ((c & 0xf0) == 0xe0) {n=2;v=c&15;min=2048;}
        else if ((c & 0xf8) == 0xf0) {n=3;v=c&7;min=65536;}
        else return -EINVAL;
        if (len-i < n) return -EINVAL;
        while (n--) {c=(unsigned char)s[i++];if ((c&0xc0)!=0x80)return -EINVAL;v=(v<<6)|(c&63);}
        if (v<min || v>0x10ffff || (v>=0xd800 && v<=0xdfff)) return -EINVAL;
    }
    return 0;
}
static int directories(void)
{
    const char *paths[]={AGENT_DATA_DIR,AGENT_DATA_DIR "/config",NEWS_DIR};
    for (unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        if (mkdir(paths[i],0755) && errno!=EEXIST) return -errno;
        struct stat st;
        if (stat(paths[i],&st) || !S_ISDIR(st.st_mode)) return -ENOTDIR;
    }
    return 0;
}
static int read_file(const char *path, char *buf, size_t cap)
{
    int fd=open(path,O_RDONLY);if(fd<0)return -errno;
    size_t used=0;int rc=0;
    while (used<cap-1) {
        ssize_t n=read(fd,buf+used,cap-1-used);
        if (n<0) {if(errno==EINTR)continue;rc=-errno;break;}
        if (!n)break;
        used+=(size_t)n;
    }
    char extra;ssize_t n;
    do {n=read(fd,&extra,1);} while(n<0 && errno==EINTR);
    if (!rc && n)rc=n<0?-errno:-EFBIG;
    if(close(fd) && !rc)rc=-errno;
    buf[used]=0;
    if (!rc && memchr(buf,0,used))rc=-EINVAL;
    return rc;
}
/* Never truncate the current file. Failed writes leave its old version intact. */
static int replace_file(const char *path, const char *text)
{
    char temp[256];if(snprintf(temp,sizeof(temp),"%s.new",path)>=(int)sizeof(temp))return -ENAMETOOLONG;
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0 && errno==EEXIST) {
        struct stat st;if(lstat(temp,&st) || !S_ISREG(st.st_mode))return -ELOOP;
        if(unlink(temp))return -errno;
        fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);
    }
    if(fd<0)return -errno;
    size_t len=strlen(text),at=0;int rc=0;
    while(at<len) {
        ssize_t n=write(fd,text+at,len-at);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){rc=n<0?-errno:-EIO;break;}
        at+=(size_t)n;
    }
    if(!rc && fsync(fd))rc=-errno;
    if(close(fd) && !rc)rc=-errno;
    char *verify=NULL;
    if(!rc) {
        verify=malloc(len+2);
        if(!verify)rc=-ENOMEM;
        else {rc=read_file(temp,verify,len+2);if(!rc && strcmp(text,verify))rc=-EIO;}
    }
    free(verify);
    if(!rc && rename(temp,path))rc=-errno;
    if(rc)unlink(temp);
    return rc;
}
int morning_profile_get(char *out,size_t cap)
{
    if(!out || cap<2)return -EINVAL;
    pthread_mutex_lock(&storage_lock);
    int rc=read_file(AGENT_USER_FILE,out,cap);
    if(rc && rc!=-EFBIG)rc=read_file(AGENT_USER_FILE ".bak",out,cap);
    pthread_mutex_unlock(&storage_lock);return rc;
}
int morning_profile_set(const char *text)
{
    int rc=utf8_text(text,MORNING_PROFILE_MAX);if(rc)return rc;
    char old[MORNING_PROFILE_MAX+1];
    pthread_mutex_lock(&storage_lock);
    rc=directories();
    if(!rc) {
        int previous=read_file(AGENT_USER_FILE,old,sizeof(old));
        if(!previous)rc=replace_file(AGENT_USER_FILE ".bak",old);
        else if(previous!=-ENOENT)rc=previous;
    }
    if(!rc)rc=replace_file(AGENT_USER_FILE,text);
    pthread_mutex_unlock(&storage_lock);return rc;
}
int morning_day(int64_t epoch,char date[11],unsigned *minute)
{
    if(epoch<1735689600LL || epoch>4102444799LL || !date)return -EINVAL;
    time_t local=(time_t)(epoch+28800);struct tm t;
    if(!gmtime_r(&local,&t) || strftime(date,11,"%Y-%m-%d",&t)!=10)return -EINVAL;
    if(minute)*minute=(unsigned)t.tm_hour*60+(unsigned)t.tm_min;
    return 0;
}
static int date_valid(const char *s)
{
    if(!s || strlen(s)!=10 || s[4]!='-' || s[7]!='-')return 0;
    for(int i=0;i<10;i++)if(i!=4 && i!=7 && !isdigit((unsigned char)s[i]))return 0;
    int y=atoi(s),m=atoi(s+5),d=atoi(s+8);
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return y>=2025 && y<=2099 && m>=1 && m<=12 && d>=1 &&
        d<=days[m-1]+(m==2 && y%4==0 && (y%100!=0 || y%400==0));
}
static int field(cJSON *o,const char *name,char *out,size_t cap)
{
    const char *s=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,name));
    if(utf8_text(s,cap-1))return -EINVAL;
    strcpy(out,s);return 0;
}
static int id_valid(const char *s)
{
    if(strlen(s)!=64)return 0;
    for(int i=0;i<64;i++)if(!isxdigit((unsigned char)s[i]))return 0;
    return 1;
}
static bool shallow_json(const char *s)
{
    unsigned depth=0;bool string=false,escape=false;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) {
        if(string){if(escape)escape=false;else if(*p=='\\')escape=true;else if(*p=='"')string=false;}
        else if(*p=='"')string=true;
        else if(*p=='{' || *p=='['){if(++depth>8)return false;}
        else if(*p=='}' || *p==']'){if(!depth)return false;depth--;}
    }
    return !depth && !string;
}
static void hash_part(uint64_t *h,const char *value)
{
    size_t n=strlen(value);char size[24];snprintf(size,sizeof(size),"%zu:",n);
    for(const unsigned char *p=(const unsigned char *)size;*p;p++){*h^=*p;*h*=UINT64_C(1099511628211);}
    for(const unsigned char *p=(const unsigned char *)value;*p;p++){*h^=*p;*h*=UINT64_C(1099511628211);}
}
static void fingerprint(struct morning_batch *b)
{
    uint64_t h=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<b->count;i++) {
        struct morning_item *v=&b->items[i];char when[24];snprintf(when,sizeof(when),"%lld",(long long)v->published_at);
        const char *parts[]={v->id,v->title,v->source,v->url,v->summary,v->relevance,when};
        for(unsigned j=0;j<sizeof(parts)/sizeof(parts[0]);j++)hash_part(&h,parts[j]);
    }
    snprintf(b->id,sizeof(b->id),"%016llx",(unsigned long long)h);
}
/* Persisted and fetched batches use the same bounded schema. Caller assigns
 * the local day; response JSON cannot choose another day or playback state. */
int morning_batch_parse(const char *json,int64_t now,struct morning_batch *out)
{
    if(!json || !out || strlen(json)>=MORNING_JSON_MAX || !shallow_json(json))return -EINVAL;
    memset(out,0,sizeof(*out));if(morning_day(now,out->date,NULL))return -EINVAL;
    const char *end; cJSON *root=cJSON_ParseWithOpts(json,&end,1);if(!root)return -EINVAL;
    int rc=-EINVAL; cJSON *schema=cJSON_GetObjectItem(root,"schema"),*items=cJSON_GetObjectItem(root,"items");
    int count=cJSON_GetArraySize(items);
    if(!cJSON_IsNumber(schema) || schema->valuedouble!=1 || !cJSON_IsArray(items) || count<0 || count>MORNING_ITEMS_MAX)goto done;
    for(int i=0;i<count;i++) {
        cJSON *v=cJSON_GetArrayItem(items,i);struct morning_item *item=&out->items[i];
        if(!cJSON_IsObject(v) || field(v,"id",item->id,sizeof(item->id)) || !id_valid(item->id) ||
           field(v,"title",item->title,sizeof(item->title)) || field(v,"source",item->source,sizeof(item->source)) ||
           field(v,"url",item->url,sizeof(item->url)) || strncmp(item->url,"https://",8) ||
           strpbrk(item->url,"\r\n\t ") || field(v,"summary",item->summary,sizeof(item->summary)) ||
           field(v,"relevance",item->relevance,sizeof(item->relevance)))goto done;
        cJSON *when=cJSON_GetObjectItem(v,"published_at");
        if(!cJSON_IsNumber(when) || when->valuedouble<now-172800 || when->valuedouble>now+300 ||
           when->valuedouble!=(double)(int64_t)when->valuedouble)goto done;
        item->published_at=(int64_t)when->valuedouble;
        for(int j=0;j<i;j++)if(!strcmp(item->id,out->items[j].id))goto done;
    }
    out->count=(unsigned)count;
    fingerprint(out);rc=0;
done:cJSON_Delete(root);return rc;
}
static int batch_path(const char *date,char path[192])
{
    if(!date_valid(date))return -EINVAL;
    return snprintf(path,192,NEWS_DIR "/brief-%s.json",date)>=192?-ENAMETOOLONG:0;
}
static cJSON *batch_json(const struct morning_batch *b)
{
    cJSON *root=cJSON_CreateObject();if(!root)return NULL;
    cJSON_AddNumberToObject(root,"schema",1);cJSON_AddStringToObject(root,"date",b->date);
    cJSON_AddStringToObject(root,"id",b->id);cJSON_AddBoolToObject(root,"asked",b->asked);
    cJSON_AddBoolToObject(root,"declined",b->declined);cJSON_AddNumberToObject(root,"played",b->played);
    cJSON *items=cJSON_AddArrayToObject(root,"items");
    for(unsigned i=0;i<b->count;i++) {
        const struct morning_item *v=&b->items[i];cJSON *item=cJSON_CreateObject();
        if(!item){cJSON_Delete(root);return NULL;}cJSON_AddItemToArray(items,item);
        cJSON_AddStringToObject(item,"id",v->id);cJSON_AddStringToObject(item,"title",v->title);
        cJSON_AddStringToObject(item,"source",v->source);cJSON_AddStringToObject(item,"url",v->url);
        cJSON_AddStringToObject(item,"summary",v->summary);cJSON_AddStringToObject(item,"relevance",v->relevance);
        cJSON_AddNumberToObject(item,"published_at",(double)v->published_at);
    }
    return root;
}
/* Restore older batches without applying today's freshness rule. */
static int load_locked(const char *date,struct morning_batch *out)
{
    char path[192];int rc=batch_path(date,path);if(rc)return rc;
    char *text=malloc(MORNING_JSON_MAX);if(!text)return -ENOMEM;
    rc=read_file(path,text,MORNING_JSON_MAX);if(rc){free(text);return rc;}
    if(!shallow_json(text)){free(text);return -EINVAL;}
    cJSON *root=cJSON_Parse(text);free(text);if(!root)return -EINVAL;
    /* Anchor parse freshness to this batch's latest item, then independently
     * validate local date and persisted state. */
    cJSON *items=cJSON_GetObjectItem(root,"items");int64_t latest=0;
    for(int i=0;i<cJSON_GetArraySize(items);i++) {
        cJSON *t=cJSON_GetObjectItem(cJSON_GetArrayItem(items,i),"published_at");
        if(cJSON_IsNumber(t) && t->valuedouble>=1735689600LL && t->valuedouble<=4102444799LL && t->valuedouble>latest)latest=(int64_t)t->valuedouble;
    }
    if(!latest && date_valid(date)) {
        struct tm d={.tm_year=atoi(date)-1900,.tm_mon=atoi(date+5)-1,.tm_mday=atoi(date+8),.tm_hour=4};
        latest=(int64_t)timegm(&d);
    }
    char *wire=cJSON_PrintUnformatted(root);rc=wire?morning_batch_parse(wire,latest,out):-ENOMEM;free(wire);
    const char *stored_date=cJSON_GetStringValue(cJSON_GetObjectItem(root,"date"));
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItem(root,"id"));
    cJSON *played=cJSON_GetObjectItem(root,"played"),*asked=cJSON_GetObjectItem(root,"asked"),*declined=cJSON_GetObjectItem(root,"declined");
    if(!rc && (!stored_date || strcmp(date,stored_date) || !id || strcmp(id,out->id) ||
       !cJSON_IsBool(asked) || !cJSON_IsBool(declined) || !cJSON_IsNumber(played) ||
       played->valuedouble<0 || played->valuedouble>out->count || played->valuedouble!=(double)(unsigned)played->valuedouble))rc=-EINVAL;
    if(!rc){strcpy(out->date,date);out->asked=cJSON_IsTrue(asked);out->declined=cJSON_IsTrue(declined);out->played=(unsigned)played->valuedouble;}
    cJSON_Delete(root);return rc;
}
int morning_batch_load(const char *date,struct morning_batch *out)
{
    if(!out)return -EINVAL;
    pthread_mutex_lock(&storage_lock);
    int rc=load_locked(date,out);pthread_mutex_unlock(&storage_lock);return rc;
}
static int save_locked(const struct morning_batch *batch)
{
    char path[192];int rc=batch_path(batch->date,path);if(rc)return rc;
    if(batch->count>MORNING_ITEMS_MAX || batch->played>batch->count)return -EINVAL;
    cJSON *root=batch_json(batch);char *text=root?cJSON_PrintUnformatted(root):NULL;
    if(!text)rc=-ENOMEM;
    else if(strlen(text)>=MORNING_JSON_MAX)rc=-EFBIG;
    else {rc=directories();if(!rc)rc=replace_file(path,text);}
    cJSON_Delete(root);free(text);return rc;
}
int morning_batch_save(const struct morning_batch *batch)
{
    if(!batch)return -EINVAL;
    pthread_mutex_lock(&storage_lock);
    int rc=save_locked(batch);pthread_mutex_unlock(&storage_lock);return rc;
}
int morning_batch_publish(const struct morning_batch *batch)
{
    if(!batch)return -EINVAL;
    struct morning_batch *old=calloc(1,sizeof(*old));if(!old)return -ENOMEM;
    pthread_mutex_lock(&storage_lock);int rc=load_locked(batch->date,old);
    if(!rc)rc=!strcmp(old->id,batch->id)?0:-EEXIST;
    else if(rc==-ENOENT)rc=save_locked(batch);
    pthread_mutex_unlock(&storage_lock);free(old);return rc;
}
int morning_batch_prune(int64_t now)
{
    char cutoff[11];if(morning_day(now-30*86400,cutoff,NULL))return -EINVAL;
    pthread_mutex_lock(&storage_lock);DIR *d=opendir(NEWS_DIR);
    if(!d){int rc=errno==ENOENT?0:-errno;pthread_mutex_unlock(&storage_lock);return rc;}
    struct dirent *entry;int rc=0;
    while((entry=readdir(d))) {
        const char *n=entry->d_name;
        if(strlen(n)!=21 || strncmp(n,"brief-",6) || strcmp(n+16,".json"))continue;
        char date[11];memcpy(date,n+6,10);date[10]=0;
        if(!date_valid(date) || strcmp(date,cutoff)>=0)continue;
        char path[192];if(batch_path(date,path))continue;
        struct stat st;if(lstat(path,&st) || !S_ISREG(st.st_mode))continue;
        if(unlink(path) && !rc)rc=-errno;
    }
    if(closedir(d) && !rc)rc=-errno;
    pthread_mutex_unlock(&storage_lock);return rc;
}
static int clean_speech(const char *text,char *clean,size_t cap)
{
    if(!text)return -EINVAL;
    size_t n=strlen(text);if(n>=cap)return -EFBIG;
    while(n && strchr(" \t\r\n",(unsigned char)*text)){text++;n--;}
    memcpy(clean,text,n);clean[n]=0;
    bool changed=true;
    while(changed && n) {
        changed=false;
        if(strchr(" \t\r\n.!?,",(unsigned char)clean[n-1])){clean[--n]=0;changed=true;continue;}
        const char *punct[]={"。","！","？","，",NULL};
        for(int i=0;punct[i];i++){size_t k=strlen(punct[i]);if(n>=k && !memcmp(clean+n-k,punct[i],k)){n-=k;clean[n]=0;changed=true;break;}}
    }
    return 0;
}
bool morning_play_request(const char *text)
{
    char clean[128];if(clean_speech(text,clean,sizeof(clean)))return false;
    const char *phrases[]={"播放今天的新闻","播放今天新闻","播放新闻","播报今天的新闻",NULL};
    for(int i=0;phrases[i];i++)if(!strcmp(clean,phrases[i]))return true;
    return false;
}
int morning_consent_parse(const char *text)
{
    char clean[128];if(clean_speech(text,clean,sizeof(clean)))return 0;
    const char *no[]={"不要","不需要","不播放","不听","不要听","不想听","不要播放","不要播放新闻","不需要播放","暂不播放","先不听","晚点再说","不用了","算了","不播","别播放","先别播放",NULL};
    const char *yes[]={"播放","要听","需要","需要播放","我要听","我想听","可以播放","开始播报","播放新闻","开始播放","好的","好",NULL};
    for(int i=0;no[i];i++)if(!strcmp(clean,no[i]))return -1;
    for(int i=0;yes[i];i++)if(!strcmp(clean,yes[i]))return 1;
    return 0;
}
bool morning_due(int64_t epoch,bool prepared)
{
    char day[11];unsigned minute;
    return !prepared && !morning_day(epoch,day,&minute) && minute>=415 && minute<1320;
}

int morning_batch_render(const struct morning_batch *batch,char *out,size_t cap)
{
    if(!batch || !out || !cap)return -EINVAL;
    cJSON *root=batch_json(batch);char *json=root?cJSON_PrintUnformatted(root):NULL;
    cJSON_Delete(root);if(!json)return -ENOMEM;
    int rc=strlen(json)<cap?0:-ENOSPC;if(!rc)strcpy(out,json);free(json);return rc;
}

int morning_item_seen(const char *id,const char *today)
{
    if(!id || !date_valid(today))return 0;
    struct morning_batch *b=calloc(1,sizeof(*b));if(!b)return -ENOMEM;
    pthread_mutex_lock(&storage_lock);DIR *d=opendir(NEWS_DIR);int found=0;
    if(d) {
        struct dirent *e;
        while(!found && (e=readdir(d))) {
            const char *n=e->d_name;if(strlen(n)!=21 || strncmp(n,"brief-",6) || strcmp(n+16,".json"))continue;
            char date[11];memcpy(date,n+6,10);date[10]=0;
            if(!date_valid(date) || strcmp(date,today)>=0)continue;
            if(!load_locked(date,b))for(unsigned i=0;i<b->count;i++)if(!strcmp(id,b->items[i].id)){found=1;break;}
        }
        closedir(d);
    }
    pthread_mutex_unlock(&storage_lock);free(b);return found;
}

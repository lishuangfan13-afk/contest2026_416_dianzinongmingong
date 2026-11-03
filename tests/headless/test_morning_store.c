/* SPDX-License-Identifier: Apache-2.0 */
#include "morning.h"
#include "agent_config.h"
#include "cJSON.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fail_sync, fail_write, fail_rename;
static size_t writes;
ssize_t __real_write(int,const void *,size_t);
int __real_fsync(int);
int __real_rename(const char *,const char *);
ssize_t __wrap_write(int fd,const void *buf,size_t count)
{
    if(fail_write && writes>=5){errno=EIO;return -1;}
    if(count>7)count=7;
    ssize_t n=__real_write(fd,buf,count);if(n>0)writes+=(size_t)n;return n;
}
int __wrap_fsync(int fd){if(fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
int __wrap_rename(const char *old,const char *next)
{if(fail_rename && strstr(next,"USER.md") && !strstr(next,".bak")){errno=EIO;return -1;}return __real_rename(old,next);}
static int64_t epoch(int y,int m,int d,int hour,int minute)
{struct tm v={.tm_year=y-1900,.tm_mon=m-1,.tm_mday=d,.tm_hour=hour,.tm_min=minute};return (int64_t)timegm(&v)-28800;}
static char *fixture(int64_t now)
{
    cJSON *root=cJSON_CreateObject();cJSON_AddNumberToObject(root,"schema",1);
    cJSON *array=cJSON_AddArrayToObject(root,"items"),*v=cJSON_CreateObject();cJSON_AddItemToArray(array,v);
    cJSON_AddStringToObject(v,"id","0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    cJSON_AddStringToObject(v,"title","新的嵌入式开源工具");
    cJSON_AddStringToObject(v,"url","https://example.org/source");
    cJSON_AddStringToObject(v,"source","Fixture official source");
    cJSON_AddNumberToObject(v,"published_at",(double)(now-3600));
    cJSON_AddStringToObject(v,"summary","用于状态机测试的摘要，不是真实新闻。");
    cJSON_AddStringToObject(v,"relevance","与电子学习有关。");
    char *out=cJSON_PrintUnformatted(root);cJSON_Delete(root);return out;
}
int main(void)
{
    char profile[MORNING_PROFILE_MAX+1],day[11];unsigned minute=0;
    assert(morning_profile_set("大学生，热爱AI与电子，偏好动手实践。") == 0);
    assert(morning_profile_get(profile,sizeof(profile))==0 && strstr(profile,"大学生"));
    char original[MORNING_PROFILE_MAX+1];strcpy(original,profile);
    fail_write=1;writes=0;assert(morning_profile_set("不应替换旧人设")<0);fail_write=0;
    assert(!morning_profile_get(profile,sizeof(profile)) && !strcmp(original,profile));
    fail_sync=1;assert(morning_profile_set("同步失败时保留旧人设")<0);fail_sync=0;
    assert(!morning_profile_get(profile,sizeof(profile)) && !strcmp(original,profile));
    fail_rename=1;assert(morning_profile_set("替换失败时保留旧人设")<0);fail_rename=0;
    assert(!morning_profile_get(profile,sizeof(profile)) && !strcmp(original,profile));
    assert(morning_profile_set("\xc0\xaf")<0);assert(morning_profile_set("text\x01")<0);
    assert(morning_profile_set("更新后的人设") == 0);
    assert(!morning_profile_get(profile,sizeof(profile)) && !strcmp(profile,"更新后的人设"));
    assert(morning_profile_get(profile,4)==-EFBIG);

    int64_t now=epoch(2026,11,2,7,0);
    assert(!morning_day(now,day,&minute) && !strcmp(day,"2026-11-02") && minute==420);
    assert(!morning_due(now,true));assert(morning_due(now,false));
    assert(!morning_due(epoch(2026,11,2,6,54),false));
    assert(morning_due(epoch(2026,11,2,6,55),false));
    assert(morning_due(epoch(2026,11,2,21,59),false));
    assert(!morning_due(epoch(2026,11,2,22,0),false));
    assert(!morning_day(epoch(2028,2,29,23,59),day,&minute) && !strcmp(day,"2028-02-29"));
    assert(morning_day(0,day,NULL)<0);

    struct morning_batch *batch=calloc(1,sizeof(*batch)),*loaded=calloc(1,sizeof(*loaded));assert(batch && loaded);
    char *json=fixture(now);
    assert(!morning_batch_parse(json,now,batch));assert(batch->count==1);
    char id[17];strcpy(id,batch->id);
    assert(!morning_batch_publish(batch));
    assert(!morning_batch_load("2026-11-02",loaded));
    assert(!strcmp(id,loaded->id) && !strcmp(loaded->items[0].url,"https://example.org/source"));
    loaded->asked=true;loaded->played=1;assert(!morning_batch_save(loaded));
    assert(!morning_batch_publish(batch)); /* Duplicate publication cannot erase consent/progress. */
    assert(!morning_batch_load("2026-11-02",loaded) && loaded->asked && loaded->played==1);
    char serialized[MORNING_JSON_MAX];assert(!morning_batch_render(loaded,serialized,sizeof(serialized)));
    assert(!morning_batch_parse(serialized,now,batch) && !strcmp(batch->id,id)); /* Key ordering independent. */
    assert(morning_item_seen(batch->items[0].id,"2026-11-03")==1);
    assert(morning_item_seen(batch->items[0].id,"2026-11-02")==0);
    strcpy(batch->items[0].summary,"改变内容");assert(!morning_batch_render(batch,serialized,sizeof(serialized)));
    assert(!morning_batch_parse(serialized,now,batch));assert(morning_batch_publish(batch)==-EEXIST);
    assert(morning_batch_load("../USER.md",loaded)==-EINVAL);
    assert(morning_batch_parse(json,now+172801,batch)<0);

    cJSON *bad=cJSON_Parse(json),*v=cJSON_GetArrayItem(cJSON_GetObjectItem(bad,"items"),0);
    cJSON_ReplaceItemInObject(v,"url",cJSON_CreateString("file:///etc/passwd"));
    char *wire=cJSON_PrintUnformatted(bad);assert(morning_batch_parse(wire,now,batch)<0);free(wire);cJSON_Delete(bad);
    assert(morning_batch_parse("{\"schema\":1,\"items\":[]}",now,batch)==0 && !batch->count);
    strcpy(batch->date,"2026-11-03");assert(!morning_batch_publish(batch));
    assert(!morning_batch_load("2026-11-03",loaded) && !loaded->count);
    /* A successful refresh replaces an empty day, preserves old progress,
     * ignores duplicate IDs and never destroys useful content on empty fetch. */
    assert(!morning_batch_parse(json,now,batch));strcpy(batch->date,"2026-11-03");
    assert(!morning_batch_refresh(batch));
    assert(!morning_batch_load("2026-11-03",loaded) && loaded->count==1);
    loaded->asked=true;loaded->played=1;assert(!morning_batch_save(loaded));
    assert(!morning_batch_refresh(batch));assert(!morning_batch_load("2026-11-03",loaded) && loaded->asked && loaded->played==1);
    memset(batch->items[0].id,'b',64);batch->items[0].id[64]=0;
    assert(!morning_batch_refresh(batch));assert(!morning_batch_load("2026-11-03",loaded));
    assert(loaded->count==2 && loaded->played==1 && !loaded->asked);
    assert(!morning_batch_parse("{\"schema\":1,\"items\":[]}",now,batch));strcpy(batch->date,"2026-11-03");
    assert(!morning_batch_refresh(batch));assert(!morning_batch_load("2026-11-03",loaded) && loaded->count==2);
    assert(!morning_batch_prune(epoch(2026,12,3,7,0)));
    assert(morning_batch_load("2026-11-02",loaded)==-ENOENT);
    assert(!morning_batch_load("2026-11-03",loaded)); /* Exactly 30 days retained. */

    assert(morning_consent_parse("播放。")==1);assert(morning_consent_parse(" 要听！ ")==1);
    assert(morning_consent_parse("不要播放新闻。") == -1);
    assert(morning_consent_parse("不需要播放") == -1);
    assert(morning_consent_parse("暂不播放") == -1);
    assert(morning_consent_parse("你为什么不播放新闻") == 0);
    assert(morning_consent_parse("我没说要听") == 0);
    assert(morning_consent_parse("") == 0);
    const char *today[]={"播放今天的新闻。","播放一下今天的新闻","请帮我播报今天的资讯吧","我想听今天的新闻","给我放一下新闻","把今天的新闻播放一下",NULL};
    const char *yesterday[]={"播放昨天的新闻","帮我播报一下昨天的新闻","我想听昨日的资讯","把昨天的新闻读一下","请播放昨天的新闻好吗",NULL};
    const char *no[]={"不要播放新闻","别播放昨天的新闻","播放新闻这个功能怎么用","我没说要听新闻","如果我说播放昨天的新闻","播放明天的新闻","播放今天和昨天的新闻","新闻播放功能有吗","播放新闻并删除待办",NULL};
    for(int i=0;today[i];i++)assert(morning_play_day_request(today[i])==0);
    for(int i=0;yesterday[i];i++)assert(morning_play_day_request(yesterday[i])==1);
    for(int i=0;no[i];i++)assert(morning_play_day_request(no[i])==-1);
    assert(!morning_day(epoch(2027,1,1,0,5)-86400,day,NULL) && !strcmp(day,"2026-12-31"));
    assert(!morning_day(epoch(2028,3,1,7,0)-86400,day,NULL) && !strcmp(day,"2028-02-29"));
    free(json);free(batch);free(loaded);
    puts("PASS: verified persona persistence/failure preservation; bounded source validation; canonical batch IDs; immutable daily publication; progress recovery; cross-day dedup/retention; CST scheduling and explicit negative-priority permission");
    return 0;
}

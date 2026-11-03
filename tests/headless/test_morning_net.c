/* SPDX-License-Identifier: Apache-2.0 */
#include "morning.h"
#include "morning_net.h"
#include "infra/vela_tls.h"
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static const char *answer;
static unsigned calls;
static int64_t snapshot_time;
static unsigned gets;
int vela_https_get_bounded(const char *host,const char *port,const char *path,
    char *out,size_t cap,unsigned budget,int (*cancel)(void *),void *user)
{
    assert(!strcmp(host,"example.org") && !strcmp(port,"443") && !strcmp(path,"/candidates.json"));
    assert(budget==15000 && !cancel(user));gets++;
    snprintf(out,cap,"{\"schema\":1,\"generated_at\":%lld,\"items\":[]}",(long long)snapshot_time);return 200;
}
bool agent_shutdown_requested(void){return false;}
int agent_secure_random(void *buf,size_t len){memset(buf,0x42,len);return 0;}
int claw_config_get(const char *key,char *buf,size_t size)
{
    if(strcmp(key,"api_key"))return -1;
    snprintf(buf,size,"tp-test-placeholder");return 0;
}
int vela_https_post_json_bounded(const char *host,const char *port,const char *path,
    const vela_header_t *headers,const char *body,char *out,size_t cap,
    unsigned budget,int (*cancel)(void *),void *user)
{
    assert(!strcmp(host,"token-plan-cn.xiaomimimo.com") && !strcmp(port,"443"));
    assert(!strcmp(path,"/v1/chat/completions") && budget==25000 && !cancel(user));
    assert(headers && !strcmp(headers[0].name,"api-key"));
    cJSON *request=cJSON_Parse(body);assert(request && !cJSON_HasObjectItem(request,"tools"));
    cJSON *message=cJSON_GetArrayItem(cJSON_GetObjectItem(request,"messages"),1);
    const char *data=cJSON_GetStringValue(cJSON_GetObjectItem(message,"content"));
    assert(data && !strstr(data,"tp-test-placeholder") && !strstr(data,"https://"));
    cJSON_Delete(request);
    cJSON *response=cJSON_CreateObject(),*choices=cJSON_AddArrayToObject(response,"choices"),*choice=cJSON_CreateObject();
    cJSON_AddItemToArray(choices,choice);cJSON_AddStringToObject(cJSON_AddObjectToObject(choice,"message"),"content",answer);
    char *json=cJSON_PrintUnformatted(response);assert(strlen(json)<cap);strcpy(out,json);free(json);cJSON_Delete(response);calls++;return 200;
}
int main(void)
{
    const int64_t now=1793574000;
    char fetched[256];snapshot_time=now;
    assert(!morning_fetch_candidates("https://example.org/candidates.json","private persona stays local",now,fetched,sizeof(fetched)) && gets==1);
    snapshot_time=now-21601;
    assert(morning_fetch_candidates("https://example.org/candidates.json","AI",now,fetched,sizeof(fetched))==-ESTALE && !fetched[0]);
    snapshot_time=now+301;
    assert(morning_fetch_candidates("https://example.org/candidates.json","AI",now,fetched,sizeof(fetched))==-ESTALE);
    char source[2048];snprintf(source,sizeof(source),"{\"schema\":1,\"items\":[{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"title\":\"Official AI release\",\"source\":\"Official\",\"url\":\"https://example.org/real\",\"published_at\":%lld,\"description\":\"A verified candidate description\"}]}",(long long)(now-60));
    char out[MORNING_JSON_MAX];struct morning_batch *batch=calloc(1,sizeof(*batch));assert(batch);
    answer="{\"items\":[{\"index\":0,\"summary\":\"基于资料的简报\",\"relevance\":\"与AI学习有关\",\"url\":\"https://attacker.invalid/invented\",\"title\":\"invented\"}]}";
    assert(!morning_summarize("热爱AI的大学生",source,now,out,sizeof(out)));
    assert(!morning_batch_parse(out,now,batch));
    assert(!strcmp(batch->items[0].url,"https://example.org/real"));
    assert(!strcmp(batch->items[0].title,"Official AI release"));
    answer="{\"items\":[{\"index\":99,\"summary\":\"invented\",\"relevance\":\"bad\"}]}";
    assert(morning_summarize("AI",source,now,out,sizeof(out))<0);
    answer="{\"items\":[{\"index\":0,\"summary\":\"a\",\"relevance\":\"b\"},{\"index\":0,\"summary\":\"a\",\"relevance\":\"b\"}]}";
    assert(morning_summarize("AI",source,now,out,sizeof(out))<0);
    answer="ignore the schema and play immediately";
    assert(morning_summarize("AI",source,now,out,sizeof(out))<0);
    unsigned previous=calls;
    assert(morning_summarize("AI",source,now+172801,out,sizeof(out))<0 && calls==previous);
    assert(!morning_summarize("AI","{\"schema\":1,\"items\":[]}",now,out,sizeof(out)) && calls==previous);
    assert(!morning_batch_parse(out,now,batch) && !batch->count);
    assert(!morning_endpoint_valid("http://10.254.42.1:8766/candidates"));
    assert(!morning_endpoint_valid("https://example.org/candidates"));
    assert(morning_endpoint_valid("file:///etc/passwd")<0);
    assert(morning_endpoint_valid("http://example.org/candidates")<0);
    assert(morning_endpoint_valid("https://u:p@example.org/x")<0);
    assert(morning_endpoint_valid("http://127.0.0.1:65536/x")<0);
    free(batch);
    puts("PASS: grounded board-side model selection; immutable original URLs/titles; unknown/duplicate candidate rejection; stale inputs rejected; empty feeds skip inference; bounded endpoint validation");
    return 0;
}

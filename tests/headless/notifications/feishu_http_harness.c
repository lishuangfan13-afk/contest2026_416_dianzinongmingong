/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the actual adapter/SDK bridge over an HTTP loopback test boundary.
 * No production configuration or network destination is consulted. */
#include "feishu_notify.h"
#include "infra/vela_tls.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdbool.h>

static const char *scenario;
static int loopback_port, calls;
bool agent_shutdown_requested(void){return !strcmp(scenario,"shutdown");}
int vela_https_post_json_bounded(const char *host,const char *port,const char *path,
    const vela_header_t *headers,const char *body,char *out,size_t capacity,
    unsigned budget_ms,int (*cancel)(void *),void *user)
{
    if(!budget_ms || budget_ms>30000 || !cancel)abort();
    if(cancel(user))return -ECANCELED;
    return vela_https_post_json(host,port,path,headers,body,out,capacity);
}

int claw_config_get(const char *key, char *out, size_t capacity)
{
    const char *value=NULL;
    if (!strcmp(key,"feishu_notify_enabled")) {
        if (!strcmp(scenario,"disabled_missing")) return -1;
        value=!strcmp(scenario,"disabled_false")?"0":"1";
    } else if (!strcmp(key,"feishu_app_id")) {
        if (!strcmp(scenario,"missing_app")) return -1;
        value="cli_synthetic_test";
    } else if (!strcmp(key,"feishu_app_secret")) {
        if (!strcmp(scenario,"missing_secret")) return -1;
        value="synthetic_test_secret_not_a_credential";
    } else if (!strcmp(key,"feishu_notify_receive_id")) {
        if (!strcmp(scenario,"missing_target")) return -1;
        value=(!strcmp(scenario,"open_id") || !strcmp(scenario,"chat_id_mismatch"))?
              "ou_synthetic_target":"oc_synthetic_target";
    } else if (!strcmp(key,"feishu_notify_receive_id_type")) {
        if (!strcmp(scenario,"default_type")) return -1;
        value=!strcmp(scenario,"unknown_type")?"email":
              (!strcmp(scenario,"open_id") || !strcmp(scenario,"open_id_mismatch"))?"open_id":"chat_id";
    } else if (!strcmp(key,"feishu_notify_auth_mode")) {
        if (!strcmp(scenario,"default_mode")) return -1;
        value=!strcmp(scenario,"oauth")?"oauth":!strcmp(scenario,"bad_mode")?"unrecognized":"legacy";
    }
    if (!value) return -1;
    if (!strcmp(scenario,"config_no_nul") && !strcmp(key,"feishu_app_id")) {
        memset(out,'x',capacity);return 0;
    }
    if (!strcmp(scenario,"config_truncated") && !strcmp(key,"feishu_app_id")) {
        memset(out,'x',capacity-1);out[capacity-1]=0;return 0;
    }
    int n=snprintf(out,capacity,"%s",value);
    return n<0 || (size_t)n>=capacity ? -1 : 0;
}

static int write_all(int fd,const char *data,size_t count)
{
    while(count) {
        ssize_t n=send(fd,data,count,0);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -EIO;
        data+=n;count-=(size_t)n;
    }
    return 0;
}

int vela_https_post_json(const char *host,const char *port,const char *path,
                        const vela_header_t *headers,const char *body,
                        char *response,size_t capacity)
{
    calls++;
    if (!host || strcmp(port,"443") || !path || !body || !capacity) return -EINVAL;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return -errno;
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons((unsigned short)loopback_port)};
    if(inet_pton(AF_INET,"127.0.0.1",&peer.sin_addr)!=1)abort();
    struct timeval timeout={.tv_sec=0,.tv_usec=200000};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(fd,(struct sockaddr *)&peer,sizeof(peer))) { int rc=-errno;close(fd);return rc; }
    const char *authorization=NULL;
    if(headers)for(const vela_header_t *h=headers;h->name;h++) {
        if(strcmp(h->name,"Authorization"))abort();
        authorization=h->value;
    }
    size_t allocated=strlen(path)+strlen(host)+strlen(body)+(authorization?strlen(authorization):0)+512;
    char *request=malloc(allocated);
    if(!request){close(fd);return -ENOMEM;}
    int used=snprintf(request,allocated,
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Test-Upstream-Host: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n",
        path,host,strlen(body));
    if(authorization)used+=snprintf(request+used,allocated-(size_t)used,"Authorization: %s\r\n",authorization);
    used+=snprintf(request+used,allocated-(size_t)used,"\r\n%s",body);
    if(used<0 || (size_t)used>=allocated)abort();
    int rc=write_all(fd,request,(size_t)used);free(request);
    size_t received=0;char *raw=malloc(capacity+8192);
    if(!raw){close(fd);return -ENOMEM;}
    while(!rc && received<capacity+8191) {
        ssize_t n=recv(fd,raw+received,capacity+8191-received,0);
        if(n<0 && errno==EINTR)continue;
        if(n<0){rc=(errno==EAGAIN||errno==EWOULDBLOCK)?-ETIMEDOUT:-errno;break;}
        if(!n)break;
        received+=(size_t)n;
    }
    raw[received]=0;close(fd);
    if(!rc) {
        char *split=strstr(raw,"\r\n\r\n");int status=0;
        if(!split || sscanf(raw,"HTTP/1.1 %d",&status)!=1)rc=-EPROTO;
        else {
            char *start=split+4;size_t length=received-(size_t)(start-raw);
            const char *declared=strstr(raw,"Content-Length: ");
            if(!declared || strtoul(declared+16,NULL,10)!=length)rc=-EPROTO;
            else if(length>=capacity)rc=-E2BIG;
            else {memcpy(response,start,length);response[length]=0;rc=status;}
        }
    }
    free(raw);return rc;
}

int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    loopback_port=atoi(argv[1]);scenario=argv[2];
    if(loopback_port<1||loopback_port>65535)return 2;
    const char *text="中文\"引号\"\\路径\n下一行";
    const char *event_id=!strcmp(scenario,"bad_event")?"contains space":"synthetic_event_001";
    char large[ZHAOXI_FEISHU_TEXT_MAX+2];
    if(!strcmp(scenario,"text_too_long")){memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;text=large;}
    int repeat=!strcmp(scenario,"repeat")?2:1;
    cJSON *results=cJSON_CreateArray();
    for(int i=0;i<repeat;i++) {
        struct zhaoxi_feishu_receipt r;
        int rc=!strcmp(scenario,"test_entry")?zhaoxi_feishu_notify_test(event_id,&r):
               !strcmp(scenario,"reminder_entry")?zhaoxi_feishu_notify_reminder(text,event_id,&r):
               zhaoxi_feishu_notify(text,event_id,&r);
        cJSON *item=cJSON_CreateObject();
        cJSON_AddNumberToObject(item,"rc",rc);cJSON_AddNumberToObject(item,"phase",r.phase);
        cJSON_AddNumberToObject(item,"http_status",r.http_status);cJSON_AddNumberToObject(item,"api_code",r.api_code);
        cJSON_AddNumberToObject(item,"send_attempted",r.send_attempted);cJSON_AddNumberToObject(item,"accepted",r.accepted);
        cJSON_AddNumberToObject(item,"delivery_unknown",r.delivery_unknown);cJSON_AddStringToObject(item,"message_id",r.message_id);
        cJSON_AddItemToArray(results,item);
    }
    cJSON *out=cJSON_CreateObject();cJSON_AddNumberToObject(out,"http_calls",calls);cJSON_AddItemToObject(out,"results",results);
    char *encoded=cJSON_PrintUnformatted(out);if(!encoded)abort();puts(encoded);free(encoded);cJSON_Delete(out);
    return 0;
}

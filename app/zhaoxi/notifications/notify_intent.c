/* SPDX-License-Identifier: Apache-2.0 */
#include "notify_service.h"
#include <string.h>
int zhaoxi_notify_request(const char *text)
{
    if(!text)return 0;
    char clean[256];size_t at=0;
    for(;*text;text++) {
        if(*text==' ' || *text=='\t' || *text=='\r' || *text=='\n')continue;
        if(at+1>=sizeof(clean))return 0;
        clean[at++]=*text;
    }
    clean[at]=0;
    const char *ends[]={"。","！",".","!",NULL};
    for(int repeat=1;repeat;) {
        repeat=0;
        for(int i=0;ends[i];i++) {
            size_t n=strlen(ends[i]);
            if(at>=n && !strcmp(clean+at-n,ends[i])){at-=n;clean[at]=0;repeat=1;break;}
        }
    }
    const char *p=clean;
    const char *prefix[]={"请帮我","帮我","请",NULL};
    for(int i=0;prefix[i];i++)if(!strncmp(p,prefix[i],strlen(prefix[i]))) {p+=strlen(prefix[i]);break;}
    struct {const char *text;int kind;} commands[]={
        {"把今天的新闻发到飞书",1},{"把今天新闻发到飞书",1},
        {"发送今天的新闻到飞书",1},{"今天的新闻发到飞书",1},
        {"把今天的资讯发到飞书",1},{"把今天的新闻推送到飞书",1},
        {"把昨天的新闻发到飞书",2},{"把昨天新闻发到飞书",2},
        {"发送昨天的新闻到飞书",2},{"昨天的新闻发到飞书",2},
        {"把昨天的资讯发到飞书",2},{"把昨天的新闻推送到飞书",2},
        {"把我的待办发到飞书",3},{"把待办发到飞书",3},
        {"发送待办到飞书",3},{"把待办清单发到飞书",3}
    };
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++)
        if(!strcmp(p,commands[i].text))return commands[i].kind;
    return 0;
}

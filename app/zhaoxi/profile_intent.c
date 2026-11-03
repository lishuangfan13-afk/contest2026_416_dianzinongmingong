/* SPDX-License-Identifier: Apache-2.0 */
#include "profile_intent.h"
#include <string.h>
static const char *space(const char *p)
{while(*p==' ' || *p=='\t')p++;return p;}
static size_t trimmed(const char *p)
{
    size_t n=strlen(p);again:;
    while(n && (p[n-1]==' ' || p[n-1]=='\t' || p[n-1]=='.' || p[n-1]=='!'))n--;
    const char *marks[]={"。","！",NULL};
    for(unsigned i=0;marks[i];i++){size_t m=strlen(marks[i]);if(n>=m && !memcmp(p+n-m,marks[i],m)){n-=m;goto again;}}
    return n;
}
int morning_profile_request(const char *text,char *content,size_t cap)
{
    if(!text || !content || cap<2)return 0;
    content[0]=0;text=space(text);
    const struct {const char *prefix;int kind;} forms[]={
        {"请记住，",1},{"请记住,",1},{"记住，",1},{"记住,",1},
        {"记住我的偏好：",1},{"记住我的偏好:",1},
        {"把我的人设改成",2},{"更新我的人设为",2},
        {"请记住",1},{"记住",1}
    };
    for(unsigned i=0;i<sizeof(forms)/sizeof(forms[0]);i++) {
        size_t prefix=strlen(forms[i].prefix);if(strncmp(text,forms[i].prefix,prefix))continue;
        const char *p=space(text+prefix);size_t n=trimmed(p);
        if(!strncmp(p,"是什么意思",strlen("是什么意思")) || !strncmp(p,"是什么",strlen("是什么")))return 0;
        if(!n || n>=cap)return 0;
        for(size_t j=0;j<n;j++)if((unsigned char)p[j]<32 || p[j]==127)return 0;
        memcpy(content,p,n);content[n]=0;return forms[i].kind;
    }
    return 0;
}
int morning_profile_decision(const char *text)
{
    if(!text)return 0;
    text=space(text);size_t n=trimmed(text);
    const char *yes[]={"确认","确认保存","保存","是的","确定",NULL};
    const char *no[]={"取消","不要保存","不保存","保持原样","算了",NULL};
    for(unsigned i=0;yes[i];i++)if(strlen(yes[i])==n && !memcmp(text,yes[i],n))return 1;
    for(unsigned i=0;no[i];i++)if(strlen(no[i])==n && !memcmp(text,no[i],n))return -1;
    return 0;
}

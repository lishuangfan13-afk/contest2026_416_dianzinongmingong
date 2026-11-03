/* SPDX-License-Identifier: Apache-2.0
 * Fixed prompt IDs, PCM24k mono s16le; never accepts a supplied file path.
 * ZXP1: magic[4], version:u32, rate:u32, channels:u16, bits:u16,
 * payload_bytes:u32, IEEE CRC32:u32, then PCM (all integers little endian).
 */
#include "local_prompts.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ZHAOXI_PROMPT_DIR
#define ZHAOXI_PROMPT_DIR "/emmc/zhaoxi/audio/prompts"
#endif
#define MAX_PCM 480000u
static const struct {const char *id, *text;} prompts[] = {
    {"hello", "你好"}, {"news_ready", ZHAOXI_NEWS_PROMPT},
    {"network_error", ZHAOXI_NETWORK_PROMPT}, {"network_connected", ZHAOXI_CONNECTED_PROMPT},
    {"uplink_choose", "选择有线连接还是无线连接"},
    {"uplink_ok", "连接成功"}, {"uplink_fail", "连接失败"},
    {"learn_wired", "请说有线连接"}, {"learn_wifi", "请说无线连接"},
    {"learn_unknown", "请说一段其他的话"}
};
const char *zhaoxi_prompt_text(const char *id)
{
    if(id)for(unsigned i=0;i<sizeof(prompts)/sizeof(prompts[0]);i++)
        if(!strcmp(id,prompts[i].id))return prompts[i].text;
    return NULL;
}
const char *zhaoxi_prompt_id(const char *text)
{
    if(text)for(unsigned i=0;i<sizeof(prompts)/sizeof(prompts[0]);i++)
        if(!strcmp(text,prompts[i].text))return prompts[i].id;
    return NULL;
}
static uint32_t le32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t crc_update(uint32_t crc,const unsigned char *p,size_t n)
{
    while(n--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));}
    return crc;
}
static int open_valid(const char *id,const char *suffix,FILE **file,size_t *bytes,
                      zhaoxi_prompt_cancel cancel,void *user)
{
    *file=NULL;*bytes=0;
    if(!zhaoxi_prompt_text(id))return -EINVAL;
    char path[160];int n=snprintf(path,sizeof(path),"%s/%s.%s",ZHAOXI_PROMPT_DIR,id,suffix);
    if(n<0 || (size_t)n>=sizeof(path))return -ENAMETOOLONG;
    FILE *f=fopen(path,"rb");if(!f)return -errno;
    struct stat info;unsigned char header[24],buffer[3200];int rc=-EBADMSG;
    if(fstat(fileno(f),&info) || !S_ISREG(info.st_mode))goto done;
    if(fread(header,1,sizeof(header),f)!=sizeof(header))goto done;
    uint32_t length=le32(header+16),expected=le32(header+20),crc=~0u;
    if(memcmp(header,"ZXP1",4) || le32(header+4)!=1 || le32(header+8)!=24000 ||
       header[12]!=1 || header[13] || header[14]!=16 || header[15] ||
       !length || length>MAX_PCM || length%2 || info.st_size!=(off_t)(24+length))goto done;
    for(size_t left=length;left;) {
        if(cancel && cancel(user)){rc=-ECANCELED;goto done;}
        size_t count=left<sizeof(buffer)?left:sizeof(buffer);
        if(fread(buffer,1,count,f)!=count)goto done;
        crc=crc_update(crc,buffer,count);left-=count;
    }
    if((crc^~0u)!=expected || fseek(f,24,SEEK_SET))goto done;
    *file=f;*bytes=length;return 0;
done:
    fclose(f);return rc;
}
int zhaoxi_prompt_check(const char *id,size_t *bytes)
{
    if(!bytes)return -EINVAL;
    FILE *f;int rc=open_valid(id,"zxp",&f,bytes,NULL,NULL);if(!rc)fclose(f);return rc;
}
int zhaoxi_prompt_stream(const char *id,zhaoxi_prompt_emit emit,
                        zhaoxi_prompt_cancel cancel,void *user,bool *handled)
{
    if(!handled)return -EINVAL;
    *handled=false;if(!emit)return -EINVAL;
    FILE *f;size_t bytes;int rc=open_valid(id,"zxp",&f,&bytes,cancel,user);
    if(rc){printf("LOCAL_PROMPT id=%s phase=validate rc=%d\n",zhaoxi_prompt_text(id)?id:"invalid",rc);return rc;}
    *handled=true;unsigned char buffer[3200];size_t sent=0;
    while(sent<bytes) {
        if(cancel && cancel(user)){rc=-ECANCELED;break;}
        size_t count=bytes-sent;if(count>sizeof(buffer))count=sizeof(buffer);
        if(fread(buffer,1,count,f)!=count){rc=-EIO;break;}
        rc=emit(buffer,count,user);if(rc)break;sent+=count;
    }
    fclose(f);
    printf("LOCAL_PROMPT id=%s bytes=%lu sent=%lu rc=%d\n",id,
           (unsigned long)bytes,(unsigned long)sent,rc);
    return rc;
}
int zhaoxi_prompt_status(char *out,size_t cap)
{
    if(!out || !cap)return -EINVAL;
    size_t used=0;
    for(unsigned i=0;i<sizeof(prompts)/sizeof(prompts[0]);i++) {
        size_t bytes;int rc=zhaoxi_prompt_check(prompts[i].id,&bytes);
        int n=snprintf(out+used,cap-used,"%s ready=%d bytes=%lu rc=%d\n",prompts[i].id,
                       !rc,(unsigned long)bytes,rc);
        if(n<0 || (size_t)n>=cap-used)return -ENOSPC;
        used+=(size_t)n;
    }
    return 0;
}
int zhaoxi_prompt_commit(const char *id)
{
    FILE *file;size_t bytes;
    int rc=open_valid(id,"part",&file,&bytes,NULL,NULL);if(rc)return rc;
    fclose(file);char from[160],to[160];
    snprintf(from,sizeof(from),"%s/%s.part",ZHAOXI_PROMPT_DIR,id);
    snprintf(to,sizeof(to),"%s/%s.zxp",ZHAOXI_PROMPT_DIR,id);
    return rename(from,to)?-errno:0;
}

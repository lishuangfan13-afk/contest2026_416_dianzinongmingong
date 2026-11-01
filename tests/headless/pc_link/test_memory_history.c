/* SPDX-License-Identifier: Apache-2.0 */
#include "agent_config.h"
#include "core/memory_store.h"
#include "core/session_mgr.h"
#include "core/context_builder.h"
#include "cJSON.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static time_t fixed_now = 1793536553;
static bool fail_append;
FILE *__real_fopen(const char *,const char *);
FILE *__wrap_fopen(const char *path,const char *mode)
{ return fail_append && !strcmp(mode,"a") && strstr(path,"/daily/") ? __real_fopen("/dev/full",mode) : __real_fopen(path,mode); }
time_t __wrap_time(time_t *out) { if(out)*out=fixed_now;return fixed_now; }
char *tool_registry_get_tools_json(void)
{
    cJSON *a=cJSON_CreateArray();
    for(int i=0;i<80;i++){ cJSON *o=cJSON_CreateObject();char name[80];snprintf(name,sizeof(name),"tool_name_%d_padding_padding",i);cJSON_AddStringToObject(o,"name",name);cJSON_AddItemToArray(a,o); }
    char *s=cJSON_PrintUnformatted(a);cJSON_Delete(a);return s;
}
size_t skill_loader_build_summary(char *out,size_t size) { int n=snprintf(out,size,"two bounded skills");return n>0&&(size_t)n<size?(size_t)n:0; }
static void write_text(const char *path,const char *s){FILE *f=fopen(path,"w");assert(f);assert(fputs(s,f)>=0);assert(fclose(f)==0);}
static void __attribute__((unused)) read_text(const char *path,char *out,size_t size){FILE *f=fopen(path,"r");assert(f);size_t n=fread(out,1,size-1,f);out[n]=0;assert(fclose(f)==0);}
static cJSON *array(const char *s){cJSON *a=cJSON_ParseWithOpts(s,NULL,1);assert(cJSON_IsArray(a));return a;}
static void prepare(void){assert(memory_store_init()==0);write_text(AGENT_MEMORY_DIR "/daily/2026-11-01.md","DAILY_CURRENT中文\n");write_text(AGENT_MEMORY_DIR "/2026-11-01.md","LEGACY_SAME_DAY\n");write_text(AGENT_MEMORY_DIR "/2026-10-31.md","LEGACY_PREVIOUS\n");}
static void histories(bool broken)
{
    char huge[2001];memset(huge,'x',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
    assert(session_append("qa","user",huge)==0);
    assert(session_append("qa","assistant","older response")==0);
    assert(session_append("qa","user","新问题\"quoted\"\n下一行")==0);
    assert(session_append("qa","assistant","最新完整回答")==0);
    char out[160];assert(session_get_history_json("qa",out,sizeof(out),20)==0);
    if(broken){assert(cJSON_Parse(out)==NULL);puts("REPRO: serialized session JSON cut mid-message");return;}
    cJSON *a=array(out);assert(cJSON_GetArraySize(a)>0);int last=cJSON_GetArraySize(a)-1;
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(a,last),"content")),"最新完整回答"));cJSON_Delete(a);
    assert(session_get_history_json("qa",out,sizeof(out),0)==0&&!strcmp(out,"[]"));
    assert(session_get_history_json("qa",out,3,20)==0&&!strcmp(out,"[]"));
    assert(session_get_history_json("qa",out,2,20)!=0&&out[0]==0);
    assert(session_get_history_json("qa",NULL,0,20)!=0);
    FILE *f=fopen(AGENT_SESSION_DIR "/tg_long.jsonl","w");assert(f);
    for(int i=0;i<8500;i++)fputc(' ',f);
    fputs("{\"role\":\"user\",\"content\":\"oversized_whitespace_record\"}\n",f);
    fputs("{\"role\":\"assistant\",\"content\":\"retained_after_oversize\"}\n",f);
    fputs("{\"role\":null,\"content\":17}\n",f);fclose(f);
    assert(session_get_history_json("long",out,sizeof(out),20)==0);a=array(out);assert(cJSON_GetArraySize(a)==1);assert(strstr(out,"retained_after_oversize"));cJSON_Delete(a);
}
static void message_context(bool broken)
{
    const char *history="[{\"role\":\"user\",\"content\":\"old question with long padding padding padding padding\"},{\"role\":\"assistant\",\"content\":\"old reply long padding padding padding\"}]";
    char out[100];int result=context_build_messages(history,"当前问题\"引号\"\n换行",out,sizeof(out));assert(result==0);
    if(broken){assert(cJSON_Parse(out)==NULL);puts("REPRO: serialized current context JSON cut mid-message");return;}
    cJSON *a=array(out);assert(cJSON_GetArraySize(a)>=1);cJSON *last=cJSON_GetArrayItem(a,cJSON_GetArraySize(a)-1);assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(last,"content")),"当前问题\"引号\"\n换行"));cJSON_Delete(a);
    assert(context_build_messages(history,"current",out,3)!=0&&!strcmp(out,"[]"));
    assert(context_build_messages("{}","short",out,sizeof(out))==0);a=array(out);assert(cJSON_GetArraySize(a)==1);cJSON_Delete(a);
    assert(context_build_messages(NULL,"short",out,sizeof(out))==0);a=array(out);cJSON_Delete(a);
    assert(context_build_messages(history,NULL,out,sizeof(out))!=0);
    assert(context_build_messages(history,"text",out,0)!=0);
}
int main(void)
{
    prepare();char out[4096];assert(memory_read_recent(out,sizeof(out),3)==0);
#ifdef EXPECT_BROKEN
    assert(!strstr(out,"DAILY_CURRENT"));puts("REPRO: canonical daily note missing from recent memory");histories(true);message_context(true);return 0;
#else
    assert(strstr(out,"DAILY_CURRENT")&&strstr(out,"LEGACY_SAME_DAY")&&strstr(out,"LEGACY_PREVIOUS"));
    assert(memory_append_today("NEW_CANONICAL_NOTE")==0);read_text(AGENT_MEMORY_DIR "/daily/2026-11-01.md",out,sizeof(out));assert(strstr(out,"NEW_CANONICAL_NOTE"));
    read_text(AGENT_MEMORY_DIR "/2026-11-01.md",out,sizeof(out));assert(!strcmp(out,"LEGACY_SAME_DAY\n"));
    assert(unlink(AGENT_MEMORY_DIR "/daily/2026-11-01.md")==0);assert(memory_read_recent(out,sizeof(out),1)==0&&strstr(out,"LEGACY_SAME_DAY"));
    for(size_t n=1;n<12;n++){unsigned char space[32];memset(space,0xa5,sizeof(space));assert(memory_read_recent((char*)space+2,n,3)==0);assert(space[1]==0xa5&&space[2+n]==0xa5);assert(strlen((char*)space+2)<n);}
    assert(memory_read_recent(NULL,0,3)!=0);assert(memory_read_recent(out,0,3)!=0);assert(memory_read_recent(out,sizeof(out),-1)!=0);
    fixed_now=1793548800;assert(memory_append_today("MIDNIGHT_CST")==0);read_text(AGENT_MEMORY_DIR "/daily/2026-11-02.md",out,sizeof(out));assert(strstr(out,"MIDNIGHT_CST"));
    fail_append=true;assert(memory_append_today("must not report saved")!=0);fail_append=false;
    histories(false);message_context(false);
    for(size_t n=1;n<=4096;n=n<16?n+1:n*2){unsigned char *space=malloc(n+4);assert(space);memset(space,0xa5,n+4);assert(context_build_system_prompt((char*)space+2,n)==0);assert(space[1]==0xa5&&space[n+2]==0xa5);assert(strlen((char*)space+2)<n);free(space);}
    assert(context_build_system_prompt(NULL,0)!=0);
    puts("PASS: canonical and legacy daily memory, checked append, CST midnight, bounded prompt, complete-message session/context trimming and invalid boundaries");
#endif
    return 0;
}

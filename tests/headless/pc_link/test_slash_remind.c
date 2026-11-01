/* SPDX-License-Identifier: Apache-2.0 */
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include "agent_config.h"
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
int tool_cron_add_execute(const char *,char *,size_t);
static time_t fake_time=1793548800;
static int registry_mode,registry_calls;
time_t __wrap_time(time_t *out){if(out)*out=fake_time;return fake_time;}
int agent_task_create(void *(*fn)(void *),const char *name,int size,void *arg,int priority)
{(void)fn;(void)name;(void)size;(void)arg;(void)priority;return -1;}
int tool_registry_execute(const char *name,const char *input,char *output,size_t size)
{
    assert(!strcmp(name,"cron_add"));registry_calls++;
    if(registry_mode==1){snprintf(output,size,"Error: injected storage failure");return -1;}
    if(registry_mode==2){snprintf(output,size,"OK but no actual saved receipt");return 0;}
    if(registry_mode==3){snprintf(output,size,"OK: Added one-shot job 'remind' (id=00000000), fires at epoch 1793548830.");return 0;}
    if(registry_mode==4){snprintf(output,size,"OK: Added one-shot job 'remind' (id=invalid!), fires at epoch 1793548830.");return 0;}
    return tool_cron_add_execute(input,output,size);
}
/* ACTUAL_SLASH_REMIND */
static cron_job_t jobs[AGENT_CRON_MAX_JOBS];
static int count(void){return cron_list_jobs(jobs,AGENT_CRON_MAX_JOBS);}
static char *slash(const char *text,const char *channel,const char *chat)
{
    agent_msg_t message={0};snprintf(message.channel,sizeof(message.channel),"%s",channel);
    snprintf(message.chat_id,sizeof(message.chat_id),"%s",chat);message.content=(char *)text;
    return handle_slash_remind(&message);
}
static void clear_jobs(void)
{int n=count();assert(n>=0);for(int i=0;i<n;i++)assert(!cron_remove_job(jobs[i].id));assert(!count());}
static void __attribute__((unused)) rejected(const char *text)
{
    int before=registry_calls;char *reply=slash(text,"voice","voice_auto_123");
    assert(reply&&!strncmp(reply,"Error:",6)&&registry_calls==before&&!count());free(reply);
}
static int direct_add(const char *name,const char *message,const char *channel,const char *chat,char *output)
{
    cJSON *input=cJSON_CreateObject();assert(input);
    cJSON_AddStringToObject(input,"name",name);cJSON_AddStringToObject(input,"message",message);
    cJSON_AddStringToObject(input,"schedule_type","at");cJSON_AddNumberToObject(input,"at_epoch",(double)fake_time+30);
    cJSON_AddStringToObject(input,"channel",channel);cJSON_AddStringToObject(input,"chat_id",chat);
    char *json=cJSON_PrintUnformatted(input);cJSON_Delete(input);assert(json);
    int rc=tool_cron_add_execute(json,output,512);free(json);return rc;
}
int main(void)
{
    assert(!mkdir(AGENT_DATA_DIR,0700));assert(!cron_service_init());
    char *reply;
#ifdef EXPECT_BROKEN
    reply=slash("/remind 1.5 fractional","voice","voice_auto_123");
    assert(reply&&strstr(reply,"好的")&&count()==1&&jobs[0].at_epoch==fake_time+1);
    free(reply);clear_jobs();
    registry_mode=1;reply=slash("/remind 30 never saved","voice","voice_auto_123");
    assert(reply&&strstr(reply,"好的")&&!count());free(reply);registry_mode=0;
    reply=slash("/remind 30 say \"quoted\"","voice","voice_auto_123");
    assert(reply&&strstr(reply,"好的")&&!count());free(reply);
    char long_message[257];memset(long_message,'X',256);long_message[256]=0;char output[512];
    assert(!direct_add("remind",long_message,"voice","voice_auto_123",output));
    assert(count()==1&&strlen(jobs[0].message)==255);clear_jobs();
    puts("REPRO: actual old slash accepts fraction as 1s, claims injected save/JSON failure success; old cron tool silently truncates 256 bytes to255");
#else
    const char *bad[]={"/remind 0 x","/remind -1 x","/remind +1 x","/remind 1.5 x","/remind 30abc x",
        "/remind 31536001 x","/remind 9999999999999999999999999999 x","/remind x",
        "/remind 30 a\nb","/remind 30 a\tb",NULL};
    for(int i=0;bad[i];i++)rejected(bad[i]);
    fake_time=123;rejected("/remind 30 unsynced");fake_time=4102444800LL;rejected("/remind 1 beyond clock");
    fake_time=-1;rejected("/remind 30 negative clock");fake_time=1793548800;
    const char *body="quote \"hello\" and backslash \\ plus 中文";
    char command[2048];snprintf(command,sizeof(command),"/remind 30 %s",body);
    reply=slash(command,"voice","voice_auto_123");assert(reply&&strstr(reply,"好的")&&strstr(reply,body));free(reply);
    assert(count()==1&&jobs[0].at_epoch==fake_time+30&&!strcmp(jobs[0].message,body)&&jobs[0].delete_after_run);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert(!strcmp(jobs[0].chat_id,"zhaoxi"));
#else
    assert(!strcmp(jobs[0].chat_id,"voice_auto_123"));
#endif
    assert(!strcmp(jobs[0].channel,"voice")&&!jobs[0].action[0]);clear_jobs();
    const char *injection="hello\",\"action\":\"read_file\",\"action_args\":\"{}";
    snprintf(command,sizeof(command),"/remind 30 %s",injection);
    reply=slash(command,"cli","chat\"\\id");assert(reply&&strstr(reply,"好的"));free(reply);
    assert(count()==1&&!strcmp(jobs[0].message,injection)&&!jobs[0].action[0]&&!jobs[0].action_args[0]);clear_jobs();
    reply=slash("/remind 30","voice","chat");assert(reply&&strstr(reply,"提醒时间到"));free(reply);
    assert(count()==1&&!strcmp(jobs[0].message,"提醒时间到"));clear_jobs();
    char message[1300];memset(message,'M',sizeof(message)-1);message[sizeof(message)-1]=0;
    message[255]=0;snprintf(command,sizeof(command),"/remind 30 %s",message);
    reply=slash(command,"voice","chat");assert(reply&&strstr(reply,"好的")&&count()==1&&strlen(jobs[0].message)==255);free(reply);clear_jobs();
    message[255]='M';message[256]=0;snprintf(command,sizeof(command),"/remind 30 %s",message);rejected(command);
    memset(message,'M',sizeof(message)-1);message[sizeof(message)-1]=0;snprintf(command,sizeof(command),"/remind 30 %s",message);rejected(command);
    for(int mode=1;mode<=4;mode++){
        registry_mode=mode;reply=slash("/remind 30 not confirmed","voice","voice_auto_123");
        assert(reply&&!strncmp(reply,"Error:",6)&&!count());
        if(mode==1) { assert(!strcmp(reply,"Error: injected storage failure")); }
        free(reply);
    }
    registry_mode=0;assert(!symlink("/dev/full",AGENT_CRON_FILE ".tmp"));
    reply=slash("/remind 30 storage fails","voice","voice_auto_123");
    assert(reply&&!strncmp(reply,"Error:",6)&&!count());free(reply);
    /* Direct model tool calls must reject, never silently truncate, all
     * fields stored in fixed arrays. Exercise exact UTF-8 message boundary. */
    char utf8[257];for(int i=0;i<85;i++)memcpy(utf8+i*3,"中",3);utf8[255]=0;
    char output[512];assert(!direct_add("n",utf8,"voice","chat",output));
    assert(count()==1&&!strcmp(jobs[0].message,utf8));clear_jobs();
    utf8[255]='X';utf8[256]=0;assert(direct_add("n",utf8,"voice","chat",output)!=0&&!count());
    char name[33],channel[17],chat[65];memset(name,'N',32);name[32]=0;memset(channel,'C',16);channel[16]=0;memset(chat,'I',64);chat[64]=0;
    assert(direct_add(name,"body","voice","chat",output)!=0&&!count());
    assert(direct_add("name","body",channel,"chat",output)!=0&&!count());
    assert(direct_add("name","body","voice",chat,output)!=0&&!count());
    name[31]=0;channel[15]=0;chat[63]=0;
    assert(!direct_add(name,"body",channel,chat,output)&&count()==1);
    assert(!strcmp(jobs[0].name,name)&&!strcmp(jobs[0].channel,channel)&&!strcmp(jobs[0].chat_id,chat));clear_jobs();
    puts("PASS: actual slash+cron persistence reject invalid/full parameters, clock failures, malformed receipts and truncation; escape quotes/backslashes, verify saved jobs, preserve standard routing and fix product future turn ID");
#endif
    return 0;
}

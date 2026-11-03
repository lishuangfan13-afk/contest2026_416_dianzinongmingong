/* SPDX-License-Identifier: Apache-2.0 */
#include "uplink_boot.h"
#include "uplink_flow.h"
#include "uplink_keywords.h"
#include "uplink_voice.h"
#include "local_prompts.h"
#include "morning_net.h"
#include "pc_link.h"
#include "agent_compat.h"
#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "infra/vela_tls.h"
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netutils/netlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static atomic_bool used=ATOMIC_VAR_INIT(false),ppp_running=ATOMIC_VAR_INIT(false);
static atomic_int ppp_result=ATOMIC_VAR_INIT(0),last_error=ATOMIC_VAR_INIT(0),selected=ATOMIC_VAR_INIT(0);
static atomic_uint attempts=ATOMIC_VAR_INIT(0);
static struct uplink_kw_model *model;
static int cancel(void *unused){(void)unused;return agent_shutdown_requested();}
bool zhaoxi_uplink_boot_used(void){return atomic_load(&used);}
static int64_t next_choice_prompt_ms;
static int64_t monotonic_ms(void)
{struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;}
static int play(void *unused,const char *id)
{
    (void)unused;bool choose=!strcmp(id,"uplink_choose");
    /* Keep listening after noise/unknown speech without repeating the full
     * question on every rejected fragment. A real connection failure still
     * announces failure and asks again immediately. */
    if(choose && monotonic_ms()<next_choice_prompt_ms)return 0;
    int rc=uplink_local_prompt(id,true);
    if(!rc)next_choice_prompt_ms=choose?monotonic_ms()+30000:0;
    return rc;
}
static int listen_choice(void *unused,int *label)
{
    (void)unused;*label=0;
    struct uplink_kw_features *features=calloc(1,sizeof(*features));if(!features)return -ENOMEM;
    int rc=uplink_voice_capture(features,true,20);
    struct uplink_kw_result result={0};
    if(!rc)rc=uplink_kw_classify(model,features,&result);
    if(!rc)*label=result.label;
    printf("UPLINK_RECOGNIZE rc=%d label=%d wired_score=%.5f wifi_score=%.5f margin=%.4f\n",
        rc,result.label,(double)result.scores[0],(double)result.scores[1],(double)result.margin);
    free(features);return rc;
}
static int interface_ready(const char *name)
{
    struct in_addr address={0};uint8_t flags=0;
    return !netlib_get_ipv4addr(name,&address) && address.s_addr &&
        !netlib_getifstatus(name,&flags) && (flags&(IFF_UP|IFF_RUNNING))==(IFF_UP|IFF_RUNNING);
}
static int probe(void)
{
    /* Probe the named public service over the selected route without an API
     * key. A 401/404 still proves DNS+TLS+HTTP, unlike merely receiving DHCP. */
    char host[128]={0},key[128]={0};
    if(claw_config_get("mimo_speech_host",host,sizeof(host)) || !host[0]) {
        if(claw_config_get("api_key",key,sizeof(key)))key[0]=0;
        snprintf(host,sizeof(host),"%s",!strncmp(key,"tp-",3) || !strncmp(key,"ttp-",4)?
            "token-plan-cn.xiaomimimo.com":"api.xiaomimimo.com");
    }
    memset(key,0,sizeof(key));
    if(strcmp(host,"api.xiaomimimo.com") && strcmp(host,"token-plan-cn.xiaomimimo.com") &&
       strcmp(host,"token-plan-sgp.xiaomimimo.com") && strcmp(host,"token-plan-ams.xiaomimimo.com"))return -EINVAL;
    int64_t epoch=0;int time_rc=morning_sntp(&epoch);
    if(!time_rc){struct timeval now={.tv_sec=(time_t)epoch};if(settimeofday(&now,NULL))time_rc=-errno;}
    printf("UPLINK_TIME rc=%d\n",time_rc);
    /* A validated NTP server reply proves an actual Internet round trip and
     * has already calibrated time. A model server's transient TLS failure
     * must not turn that healthy selected uplink into a connection failure. */
    if(!time_rc){puts("UPLINK_PROBE method=sntp rc=0");return 0;}
    char *response=calloc(1,4096);if(!response)return -ENOMEM;
    int status=vela_https_get_bounded(host,"443","/v1/models",response,4096,12000,cancel,NULL);
    free(response);
    printf("UPLINK_PROBE http=%d\n",status);
    return status>=200 && status<500?0:status<0?status:-ENETUNREACH;
}
static void *run_ppp(void *unused)
{
    (void)unused;int rc=zhaoxi_pc_link_boot();
    atomic_store(&ppp_result,rc);atomic_store(&ppp_running,false);return NULL;
}
static void stop_ppp(void)
{
    if(!atomic_load(&ppp_running))return;
    /* PPP owns the console/routes until its complete existing cleanup ends.
     * Never ask a new question/start a second transport over that owner. */
    while(atomic_load(&ppp_running)){zhaoxi_pc_link_request_stop();usleep(100000);}
}
static int connect_choice(void *unused,int label)
{
    (void)unused;
    unsigned attempt=atomic_fetch_add(&attempts,1)+1;
    atomic_store(&selected,label);int rc=0;
    if(label==UPLINK_KW_WIFI) {
        puts("UPLINK_WIFI_SELECTED");fflush(stdout);
        if(atomic_load(&ppp_running))stop_ppp();
        if(claw_config_set("uplink_mode","wifi"))rc=-EIO;
        if(!rc)rc=network_wifi_reconnect();
        if(!rc && !interface_ready("wlan0"))rc=-ENETUNREACH;
        if(!rc)rc=probe();
        if(!rc && !interface_ready("wlan0"))rc=-ENETDOWN;
    } else if(label==UPLINK_KW_WIRED) {
        /* This is a new explicit wired selection, not an automatic fallback. */
        /* Do not issue BES SIOCSIFFLAGS here: its ifdown can block after an
         * association. PPP installs the verified /1 routes and is explicitly
         * checked below, so wired Internet traffic uses the PC even after a
         * prior failed Wi-Fi attempt. A cold wired boot never joins Wi-Fi. */
        if(claw_config_set("uplink_mode","pc"))rc=-EIO;
        if(!rc) {
            atomic_store(&ppp_result,0);atomic_store(&ppp_running,true);
            printf("UPLINK_WIRED_REQUEST id=%u\n",attempt);fflush(stdout);
            if(agent_task_create(run_ppp,"uplink_ppp",32768,NULL,30)) {
                atomic_store(&ppp_running,false);rc=-EIO;
            }
        }
        while(!rc && atomic_load(&ppp_running) && !zhaoxi_pc_link_ready() && !cancel(NULL))usleep(100000);
        if(cancel(NULL))rc=-ECANCELED;
        if(!rc && (!atomic_load(&ppp_running) || !zhaoxi_pc_link_ready() || !interface_ready("ppp0")))
            rc=atomic_load(&ppp_result)?atomic_load(&ppp_result):-ENETUNREACH;
        if(!rc)rc=probe();
        if(!rc && (!atomic_load(&ppp_running) || !zhaoxi_pc_link_ready() || !interface_ready("ppp0")))rc=-ENETDOWN;
        if(rc)stop_ppp();
    } else rc=-EINVAL;
    atomic_store(&last_error,rc);
    printf("UPLINK_CONNECT mode=%d attempt=%u rc=%d\n",label,attempt,rc);fflush(stdout);
    return rc;
}
int zhaoxi_uplink_boot_dialog(bool pc_owned)
{
    char enabled[10]={0};atomic_store(&used,false);
    /* Explicit legacy PC launch already selected and established PPP. */
    if(pc_owned)return 0;
    if(claw_config_get("uplink_choice_enabled",enabled,sizeof(enabled)) ||
       (strcmp(enabled,"1") && strcmp(enabled,"true")))return 0;
    atomic_store(&used,true);atomic_store(&selected,0);atomic_store(&last_error,0);next_choice_prompt_ms=0;
    /* Fail closed if assets/templates are absent. Never secretly connect to
     * obtain ASR/TTS before the user has selected a transport. */
    const char *ids[]={"uplink_choose","uplink_ok","uplink_fail"};
    int rc=0;size_t bytes;
    for(unsigned i=0;!rc && i<3;i++)rc=zhaoxi_prompt_check(ids[i],&bytes);
    model=calloc(1,sizeof(*model));
    if(!model && !rc)rc=-ENOMEM;
    if(!rc)rc=uplink_voice_load(model);
    if(!rc && !uplink_kw_ready(model))rc=-EAGAIN;
    if(!rc) {
        const struct uplink_flow_io io={NULL,play,listen_choice,connect_choice,cancel};
        rc=uplink_flow_run(&io);
    }
    free(model);model=NULL;
    if(rc)stop_ppp();
    atomic_store(&last_error,rc);printf("UPLINK_BOOT_DONE rc=%d\n",rc);return rc;
}
void zhaoxi_uplink_boot_stop(void)
{if(atomic_load(&used) && atomic_load(&ppp_running))zhaoxi_pc_link_request_stop();}
int zhaoxi_uplink_boot_status(char *out,size_t cap)
{
    if(!out || !cap)return -EINVAL;
    int n=snprintf(out,cap,"UPLINK_BOOT active=%d selected=%d attempts=%u ppp_running=%d last_rc=%d",
        atomic_load(&used),atomic_load(&selected),atomic_load(&attempts),atomic_load(&ppp_running),atomic_load(&last_error));
    return n<0 || (size_t)n>=cap?-ENOSPC:0;
}

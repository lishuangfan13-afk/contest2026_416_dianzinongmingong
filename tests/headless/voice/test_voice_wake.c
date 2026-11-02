/* SPDX-License-Identifier: Apache-2.0 */
#include "voice/voice_wake.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static voice_wake_result_t check(voice_wake_t *wake,const char *text,uint64_t now,const char *expected)
{
    char command[512];memset(command,0xa5,sizeof(command));
    voice_wake_result_t result=voice_wake_process(wake,text,now,command,sizeof(command));
    assert(!strcmp(command,expected));return result;
}
int main(void)
{
    voice_wake_t wake=VOICE_WAKE_INITIALIZER;
    assert(!voice_wake_poll(&wake,0));
    assert(check(&wake,"今天天气怎么样",1,"")==VOICE_WAKE_IGNORE);
    assert(check(&wake,"我说你好，xixi",2,"")==VOICE_WAKE_IGNORE);
    assert(check(&wake,"请解释 Hello xixi 是什么意思",3,"")==VOICE_WAKE_IGNORE);
    const char *invalid[]={"朝夕","你好朝夕","小朝夕","xixi","HelloWorld xixi","HelloWorldxixi",
        "Hello xixixyz","你好xixi2","你好xixi_test",NULL};
    for(int i=0;invalid[i];i++) assert(check(&wake,invalid[i],4,"")==VOICE_WAKE_IGNORE);
    const char *homophones[]={"夕夕", "西西", "希希", "嘻嘻", "熙熙", "兮兮",
        "汐汐", "曦曦", "溪溪", "惜惜", "锡锡", "细细", "喜喜", "茜茜", "xixi", "Xi Xi", NULL};
    for(int i=0;homophones[i];i++) {
        char phrase[128];snprintf(phrase,sizeof(phrase),"你好，%s。",homophones[i]);
        assert(check(&wake,phrase,4,"")==VOICE_WAKE_ONLY);voice_wake_reset(&wake);
    }
    const char *old_names[]={"你好openvela","Hello openvela","你好Open文了","你好夕夕2",
        "你好西西_test","你好夕阳","你好西安","你好嘻哈",NULL};
    for(int i=0;old_names[i];i++)assert(check(&wake,old_names[i],4,"")==VOICE_WAKE_IGNORE);
    const char *asr_invalid[]={"你好","你好Open文","你好Open文章","你好西西2",
        "你好西西_test","我说你好西西","西西",NULL};
    for(int i=0;asr_invalid[i];i++)assert(check(&wake,asr_invalid[i],4,"")==VOICE_WAKE_IGNORE);
    assert(check(&wake,"你好，西西。",5,"")==VOICE_WAKE_ONLY);
    voice_wake_reset(&wake);
    assert(check(&wake,"你好，夕夕，一加一等于几",5,"一加一等于几")==VOICE_WAKE_COMMAND);
    voice_wake_reset(&wake);
    assert(check(&wake,"Helloxixi",5,"")==VOICE_WAKE_ONLY);
    voice_wake_reset(&wake);
    assert(check(&wake,"HELLOXIXI",6,"")==VOICE_WAKE_ONLY);
    voice_wake_reset(&wake);
    assert(check(&wake,"　“你好，Xi Xi。” \t",1000,"")==VOICE_WAKE_ONLY);
    assert(voice_wake_poll(&wake,1000));
    assert(check(&wake,"现在几点了？",2000,"现在几点了？")==VOICE_WAKE_COMMAND);
    assert(voice_wake_poll(&wake,60999));
    assert(!voice_wake_poll(&wake,61000));
    assert(check(&wake,"一加一等于几",61000,"")==VOICE_WAKE_IGNORE);
    assert(check(&wake,"HELLO，XIXI，记一下：明天带书。",62000,"记一下：明天带书。")==VOICE_WAKE_COMMAND);
    assert(check(&wake,"再见。",62001,"")==VOICE_WAKE_SLEEP);
    assert(!voice_wake_poll(&wake,62001));
    assert(check(&wake,"再见",62002,"")==VOICE_WAKE_IGNORE);
    assert(check(&wake,"停止监听",62003,"")==VOICE_WAKE_STOP);
    assert(check(&wake,"你好，xixi，停止监听。",62004,"")==VOICE_WAKE_STOP);
    assert(check(&wake,"你好,xixi, 如何停止监听？",63000,"如何停止监听？")==VOICE_WAKE_COMMAND);
    assert(check(&wake,"结束对话",63001,"")==VOICE_WAKE_SLEEP);
    assert(check(&wake,"Hello xixi",64000,"")==VOICE_WAKE_ONLY);
    assert(check(&wake,"休息吧",64001,"")==VOICE_WAKE_SLEEP);

    assert(check(&wake,"你好xixi",100000,"")==VOICE_WAKE_ONLY);
    voice_wake_set_busy(&wake,true,101000);
    assert(voice_wake_poll(&wake,999999));
    voice_wake_set_busy(&wake,false,999999);
    assert(wake.deadline_ms==1059999 && voice_wake_poll(&wake,1059998));
    assert(!voice_wake_poll(&wake,1059999));
    assert(check(&wake,"Hello, xi xi",2000000,"")==VOICE_WAKE_ONLY);
    voice_wake_set_busy(&wake,false,2059000);
    assert(!voice_wake_poll(&wake,2060000)); /* status polling cannot extend idle */

    voice_wake_set_enabled(&wake,false);voice_wake_reset(&wake);
    assert(!wake.enabled && voice_wake_poll(&wake,0));
    assert(check(&wake,"自由调试输入",1,"自由调试输入")==VOICE_WAKE_COMMAND);
    voice_wake_set_enabled(&wake,true);assert(!voice_wake_poll(&wake,1));
    char small[2]={'x','x'};
    assert(voice_wake_process(&wake,"Hello xixi，abc",3,small,sizeof(small))==VOICE_WAKE_REJECTED && !small[0]);
    assert(voice_wake_process(&wake,"text",3,small,0)==VOICE_WAKE_REJECTED);
    assert(check(&wake," \t，。",4,"")==VOICE_WAKE_IGNORE);
    voice_wake_reset(&wake);
    assert(check(&wake,"\xc2\xa0Hello XIXI！",5,"")==VOICE_WAKE_ONLY);
    voice_wake_reset(&wake);
    assert(check(&wake,"你好，xixi现在几点了",6,"现在几点了")==VOICE_WAKE_COMMAND);
    voice_wake_reset(&wake);
    assert(check(&wake,"你好xixi",UINT64_MAX-10,"")==VOICE_WAKE_ONLY && wake.deadline_ms==UINT64_MAX);
    voice_wake_reset(&wake);
    char alias[80]="你好，xixi，记一下明天带书";
    assert(voice_wake_process(&wake,alias,7,alias,sizeof(alias))==VOICE_WAKE_COMMAND);
    assert(!strcmp(alias,"记一下明天带书"));
    puts("PASS: anchored ASR wake phrases, Chinese whitespace/punctuation, same-sentence command, idle expiry, busy renewal, local sleep/stop, bypass switch and bounds");
    return 0;
}

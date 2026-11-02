/* SPDX-License-Identifier: Apache-2.0 */
/* Actual auto-worker and VAD with synthetic PCM and explicit API/audio
 * substitutes. This proves local state/endpoint logic, not real ASR quality. */
#define main manual_capture_tests_main
#include "test_capture_order.c"
#undef main

struct audio_playback {int active;};
static struct audio_playback playback;
static atomic_int cue_counts[9];
static int tts_empty;
static int tts_calls;
static atomic_int block_greeting, greeting_entered, greeting_release;
static int fail_greeting;
static char last_tts_text[128];

void voice_channel_auto_feedback(int event)
{
    assert(!s_auto.capture && atomic_load(&capture.phase)==0);
    assert(event>0 && event<9);atomic_fetch_add(&cue_counts[event],1);
    if(event==VOICE_AUTO_FEEDBACK_READY && atomic_load(&fake_input_pattern)==3 &&
       atomic_load(&reads)>100) {
        int unset=-1;
        atomic_compare_exchange_strong(&fake_followup_begin,&unset,atomic_load(&reads));
    }
}
audio_playback_t *audio_playback_open(const char *path,unsigned rate,unsigned channels,unsigned bits)
{
    (void)path;(void)rate;(void)channels;(void)bits;
    assert(!s_auto.capture && atomic_load(&capture.phase)==0);
    playback.active=1;return &playback;
}
int audio_playback_write(audio_playback_t *pb,const void *pcm,size_t size)
{
    assert(pb==&playback && pcm && !s_auto.capture);
    char state[768];assert(voice_channel_auto_status(state,sizeof(state))==0);
    assert(strstr(state,"\"output_active\":true")&&strstr(state,"\"output_playing\":true"));
    return (int)size;
}
void audio_playback_close(audio_playback_t *pb)
{assert(pb==&playback && !s_auto.capture);playback.active=0;}
int voice_tts_speak_stream(const char *text,voice_tts_chunk_cb cb,void *arg)
{
    assert(text && !s_auto.capture);
    tts_calls++;
    snprintf(last_tts_text,sizeof(last_tts_text),"%s",text);
    bool greeting=!strcmp(text,"你好");
    if(greeting) {
        atomic_store(&greeting_entered,1);
        while(atomic_load(&block_greeting) && !atomic_load(&greeting_release))usleep(1000);
        if(voice_channel_tts_stream_error())return -ECANCELED;
        if(fail_greeting)return -EIO;
    }
    char output_state[768];assert(voice_channel_auto_status(output_state,sizeof(output_state))==0);
    assert(strstr(output_state,"\"output_active\":true"));
    if(tts_empty)return 0;
    if(s_auto.enabled) {
        char state[512];voice_channel_auto_status(state,sizeof(state));
        assert(strstr(state,"正在合成语音") || (greeting && strstr(state,"正在播报唤醒应答")));
    }
    unsigned char pcm[640]={0};cb(pcm,sizeof(pcm),0,arg);
    if(s_auto.enabled) {
        char state[512];voice_channel_auto_status(state,sizeof(state));
        /* Enqueueing a small first fragment does not mean DMA has started.
         * The dedicated PCM consumer owns that state transition. */
        assert(strstr(state,"正在合成语音") || strstr(state,"正在播放回答") || (greeting && strstr(state,"正在播报唤醒应答")));
    }
    usleep(20000);cb(NULL,0,1,arg);return 0;
}
static void wait_count(atomic_int *value,int target)
{
    for(int i=0;i<5000 && atomic_load(value)<target;i++)usleep(1000);
    assert(atomic_load(value)>=target);
}
static void stop_auto(void)
{
    assert(voice_channel_auto_stop()==0);
    pthread_join(auto_test_thread,NULL);
    assert(!s_auto.running && !s_auto.enabled && !s_auto.capture);
    assert(opens==closes);
}
static void test_vad_rules(void)
{
    struct local_vad v={0};
    for(int i=0;i<50;i++)assert(local_vad_frame(&v,30,500)==0);
    for(int i=0;i<1000;i++)assert(local_vad_frame(&v,30,500)==0);
    assert(!v.active);
    for(int i=0;i<3;i++)local_vad_frame(&v,2000,500);
    assert(v.active);
    int event=0;
    for(int i=0;i<AUTO_SILENCE_FRAMES;i++)event=local_vad_frame(&v,30,500);
    assert(event==-1); /* short clap must not upload */
    memset(&v,0,sizeof(v));
    for(int i=0;i<50;i++)local_vad_frame(&v,30,500);
    for(int i=0;i<15;i++)local_vad_frame(&v,2000,500);
    assert(v.active);
    /* A one-second mid-sentence pause does not submit half a command. */
    for(int i=0;i<50;i++)assert(local_vad_frame(&v,30,500)==0);
    for(int i=0;i<15;i++)assert(local_vad_frame(&v,2000,500)==0);
    for(int i=0;i<AUTO_SILENCE_FRAMES;i++)event=local_vad_frame(&v,30,500);
    assert(event==2);
    memset(&v,0,sizeof(v));
    for(int i=0;i<50;i++)local_vad_frame(&v,30,500);
    for(int i=0;i<3;i++)local_vad_frame(&v,2000,50);
    for(int i=0;i<50;i++)event=local_vad_frame(&v,2000,50);
    assert(event==3); /* oversized command rejected rather than executed */
}
#ifndef AUTO_VOICE_TESTS_MAIN
#define AUTO_VOICE_TESTS_MAIN main
#endif
int AUTO_VOICE_TESTS_MAIN(void)
{
    assert(voice_channel_init()==0);test_vad_rules();
    char initial[768];assert(voice_channel_auto_status(initial,sizeof(initial))==0);
    assert(strstr(initial,"\"wake_enabled\":true")&&strstr(initial,"\"enabled\":false"));
    reset_fixture();allow_bus=1;
    assert(voice_channel_auto_start()==0);
    wait_count(&reads,300);
    assert(atomic_load(&fake_asr_calls)==0 && atomic_load(&fake_bus_messages)==0);
    assert(voice_channel_start()==-EBUSY);
    /* Any unrelated playback first closes capture; it doesn't complete a
     * pending turn, and capture restarts automatically after cooldown. */
    assert(voice_channel_speak("fixture unrelated reminder")==0);
    wait_count(&reads,380);
    assert(atomic_load(&fake_asr_calls)==0);
    stop_auto();

    /* Product announcements reserve the actual capture worker, while normal
     * voice and repeated reservations cannot overlap the codec. */
    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    assert(voice_channel_auto_start()==0);wait_count(&reads,100);
    assert(voice_channel_auto_output_begin(true)==0);
    assert(!s_auto.capture && s_auto.offering);
    assert(voice_channel_auto_output_begin(true)==-EBUSY);
    assert(voice_channel_speak_auto_owned("fixture announcement")==0);
    voice_channel_auto_output_end(false);
    assert(voice_channel_auto_news_consent(true)==0);
    wait_count(&reads,160);
    char consent_state[1024];voice_channel_auto_status(consent_state,sizeof(consent_state));
    assert(strstr(consent_state,"等待新闻播放许可") && strstr(consent_state,"\"accepting_commands\":true"));
    assert(voice_channel_auto_news_consent(false)==0);
    stop_auto();assert(!s_auto.news_consent && !s_auto.offering);

    reset_fixture();allow_bus=1;atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_bus_messages,1);
    assert(atomic_load(&fake_asr_calls)==1 && s_auto.awaiting && !s_auto.capture);
    int before=atomic_load(&reads);usleep(100000);
    assert(atomic_load(&reads)==before);
    assert(voice_channel_speak("unrelated reminder")==0);
    assert(s_auto.awaiting); /* only explicit final reply ends this turn */
    char first_id[32];snprintf(first_id,sizeof(first_id),"%s",last_chat_id);
    assert(voice_channel_auto_reply_is_active(first_id));
    voice_channel_auto_reply_done_for("voice_auto_stale");assert(s_auto.awaiting);
    voice_channel_auto_reply_done_for(first_id);
    wait_count(&reads,before+60);
    stop_auto();

    /* Reminder playback interrupts a 600ms prefix. Neither it nor the
     * remaining voice tail may be interpreted as a partial command. */
    reset_fixture();allow_bus=1;atomic_store(&fake_input_pattern,4);
    assert(voice_channel_auto_start()==0);wait_count(&reads,110);
    assert(voice_channel_speak("interrupt spoken command")==0);
    wait_count(&reads,260);
    assert(atomic_load(&fake_asr_calls)==0 && atomic_load(&fake_bus_messages)==0);
    stop_auto();

    /* Start the follow-up immediately after READY. An extra 1s calibration
     * here would consume this entire 600ms utterance. */
    reset_fixture();allow_bus=1;atomic_store(&fake_input_pattern,3);
    assert(voice_channel_auto_start()==0);wait_count(&fake_bus_messages,1);
    voice_channel_auto_reply_done_for(last_chat_id);
    wait_count(&fake_bus_messages,2);
    assert(atomic_load(&fake_asr_calls)==2);
    stop_auto();

    reset_fixture();allow_bus=1;atomic_store(&fake_input_pattern,2);
    int rejected_before=atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_TOO_LONG]);
    assert(voice_channel_auto_start()==0);wait_count(&reads,450);
    assert(atomic_load(&fake_bus_messages)==0 && atomic_load(&fake_asr_calls)==0);
    assert(atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_TOO_LONG])==rejected_before+1);
    stop_auto();

    reset_fixture();allow_bus=1;fail_asr=1;atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_asr_calls,1);
    wait_count(&reads,220);assert(atomic_load(&fake_bus_messages)==0);
    stop_auto();
    /* Stop during ASR: no late error cue or queued work after cancellation. */
    reset_fixture();allow_bus=1;fail_asr=1;atomic_store(&fake_input_pattern,1);
    atomic_store(&block_asr,1);
    int cues_before=atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_ASR_ERROR]);
    assert(voice_channel_auto_start()==0);wait_count(&fake_asr_calls,1);
    assert(voice_channel_auto_stop()==0);atomic_store(&asr_release,1);
    pthread_join(auto_test_thread,NULL);
    assert(!s_auto.running && !s_auto.capture && atomic_load(&fake_bus_messages)==0);
    assert(atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_ASR_ERROR])==cues_before);

    /* Timeouts stop the automatic loop; a cancelled ID cannot release a later
     * turn even if its model/outbound operation finishes after restart. */
    reset_fixture();allow_bus=1;atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_bus_messages,1);
    char expired_id[32];snprintf(expired_id,sizeof(expired_id),"%s",last_chat_id);
    pthread_mutex_lock(&s_auto.lock);
    s_auto.reply_started=auto_now_ms()-AUTO_REPLY_TIMEOUT_MS-1;
    pthread_mutex_unlock(&s_auto.lock);
    pthread_join(auto_test_thread,NULL);
    assert(!s_auto.enabled && !s_auto.running);
    assert(!voice_channel_auto_reply_is_active(expired_id));
    reset_fixture();allow_bus=1;atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_bus_messages,1);
    assert(strcmp(expired_id,last_chat_id));
    voice_channel_auto_reply_done_for(expired_id);assert(s_auto.awaiting);
    int failed_cues=atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_DISPATCH_ERROR]);
    voice_channel_auto_reply_failed_for(expired_id,VOICE_AUTO_FEEDBACK_DISPATCH_ERROR);
    assert(s_auto.awaiting && atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_DISPATCH_ERROR])==failed_cues);
    voice_channel_auto_reply_failed_for(last_chat_id,VOICE_AUTO_FEEDBACK_DISPATCH_ERROR);
    assert(!s_auto.awaiting && atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_DISPATCH_ERROR])==failed_cues+1);
    stop_auto();
    tts_empty=1;assert(voice_channel_speak("missing audio")==-ENODATA);tts_empty=0;
    char status[512];assert(voice_channel_auto_status(status,sizeof(status))==0);
    assert(strstr(status,"\"enabled\":false"));
    assert(strstr(status,"\"accepting_speech\":false"));
    assert(strstr(status,"\"output_active\":false"));

    /* Waiting ASR text never reaches the model bus, even though the mock ASR
     * has successfully recognized an unrelated bystander's utterance. */
    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    fake_batch_text="这是旁人讲话";atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_asr_calls,1);wait_count(&reads,250);
    assert(!atomic_load(&fake_bus_messages));
    assert(voice_channel_auto_status(status,sizeof(status))==0);
    assert(strstr(status,"\"awake\":false")&&strstr(status,"\"accepting_commands\":false"));stop_auto();

    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    fake_batch_text="你好，西西。";atomic_store(&fake_input_pattern,1);int tts_before=tts_calls;
    assert(voice_channel_auto_start()==0);wait_count(&fake_asr_calls,1);wait_count(&reads,250);
    assert(!atomic_load(&fake_bus_messages)&&tts_calls==tts_before+1);
    assert(!strcmp(last_tts_text,"你好"));
    voice_channel_auto_status(status,sizeof(status));assert(strstr(status,"等待提问"));
    bool awake=false,wake_enabled=false;assert(voice_channel_auto_wake_get_state(&wake_enabled,&awake)==0&&wake_enabled&&awake);stop_auto();

    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    tts_before=tts_calls;
    fake_batch_text="HeLLo，XIXI，一加一等于几";atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_bus_messages,1);
    assert(tts_calls==tts_before+1 && !strcmp(last_tts_text,"你好"));
    assert(!strcmp(last_content,"一加一等于几")&&s_auto.awaiting&&s_auto.wake.busy);
    pthread_mutex_lock(&s_auto.lock);s_auto.wake.deadline_ms=(uint64_t)auto_now_ms()-1;pthread_mutex_unlock(&s_auto.lock);
    assert(voice_channel_auto_wake_get_state(NULL,&awake)==0&&awake); /* processing pauses expiry */
    voice_channel_auto_reply_done_for(last_chat_id);
    pthread_mutex_lock(&s_auto.lock);assert(!s_auto.wake.busy&&s_auto.wake.deadline_ms>(uint64_t)auto_now_ms()+59000);pthread_mutex_unlock(&s_auto.lock);
    stop_auto();

    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    fake_batch_text="你好，xixi，再见";atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);wait_count(&fake_asr_calls,1);wait_count(&reads,250);
    assert(!atomic_load(&fake_bus_messages));assert(voice_channel_auto_wake_get_state(NULL,&awake)==0&&!awake);stop_auto();

    /* Questions stay blocked until greeting playback finishes. Cancellation
     * while cloud TTS is pending must not play late audio or dispatch text. */
    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    fake_batch_text="你好，xixi，一加一等于几";atomic_store(&fake_input_pattern,1);
    atomic_store(&greeting_entered,0);atomic_store(&greeting_release,0);atomic_store(&block_greeting,1);
    assert(voice_channel_auto_start()==0);wait_count(&greeting_entered,1);
    assert(!s_auto.capture && !atomic_load(&fake_bus_messages));
    voice_channel_auto_status(status,sizeof(status));
    assert(strstr(status,"正在播报唤醒应答") && strstr(status,"\"accepting_commands\":false"));
    assert(voice_channel_auto_stop()==0);atomic_store(&greeting_release,1);
    pthread_join(auto_test_thread,NULL);atomic_store(&block_greeting,0);
    assert(!s_auto.running && !s_auto.capture && !playback.active && !atomic_load(&fake_bus_messages));
    assert(opens==closes);

    /* Failed greeting returns to the wake gate, with no question dispatched. */
    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    fake_batch_text="你好，xixi，一加一等于几";atomic_store(&fake_input_pattern,1);
    fail_greeting=1;
    int wake_errors=atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_WAKE_ERROR]);
    assert(voice_channel_auto_start()==0);wait_count(&reads,250);
    assert(!atomic_load(&fake_bus_messages));
    voice_channel_auto_status(status,sizeof(status));
    assert(strstr(status,"\"awake\":false") && strstr(status,"等待唤醒词"));
    assert(atomic_load(&cue_counts[VOICE_AUTO_FEEDBACK_WAKE_ERROR])==wake_errors+1);
    stop_auto();fail_greeting=0;

    /* A short VAD trigger rejected before ASR must release busy while the
     * capture loop stays open, otherwise the idle window never expires. */
    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    atomic_store(&fake_input_pattern,5);
    unsigned rejected_base=s_auto.rejected;
    assert(voice_channel_auto_start()==0);wait_count(&reads,75);
    pthread_mutex_lock(&s_auto.lock);
    char wake_command[80];
    assert(voice_wake_process(&s_auto.wake,"你好，xixi",(uint64_t)auto_now_ms(),wake_command,sizeof(wake_command))==VOICE_WAKE_ONLY);
    pthread_mutex_unlock(&s_auto.lock);
    wait_count(&reads,200);
    pthread_mutex_lock(&s_auto.lock);
    assert(s_auto.rejected>rejected_base && s_auto.capture);
    assert(!s_auto.wake_processing && !s_auto.wake.busy);
    assert(s_auto.wake.deadline_ms>(uint64_t)auto_now_ms()+59000);
    s_auto.wake.deadline_ms=(uint64_t)auto_now_ms()-1;
    pthread_mutex_unlock(&s_auto.lock);
    assert(voice_channel_auto_wake_get_state(NULL,&awake)==0&&!awake);
    assert(!atomic_load(&fake_asr_calls)&&!atomic_load(&fake_bus_messages));stop_auto();

    reset_fixture();allow_bus=1;assert(voice_channel_auto_wake_set_enabled(true)==0);
    fake_batch_text="停止监听";atomic_store(&fake_input_pattern,1);
    assert(voice_channel_auto_start()==0);pthread_join(auto_test_thread,NULL);
    assert(!s_auto.running&&!s_auto.enabled&&!s_auto.capture&&!atomic_load(&fake_bus_messages));
    char tiny[4];assert(voice_channel_auto_status(tiny,sizeof(tiny))==-ENOSPC);
    puts("auto voice passed: 1s continuation merged, 1.2s endpoint, oversized whole-turn and tail rejection, cue/capture exclusion, cancelled-ASR silence, generation-safe replies, timeout stops, true synthesis/playback stages, empty TTS failure");
    puts("ASR wake worker passed: xixi prefixes, bystander discarded, wake-only spoken greeting, greeting-before-question, greeting cancellation/failure, stripped same-sentence command, pending-work pause and reply renewal, rejected short VAD releases busy and expires, sleep and actual stop");
    return 0;
}

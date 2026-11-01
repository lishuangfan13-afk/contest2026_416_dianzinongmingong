/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
static atomic_bool __attribute__((unused)) shutdown_flag,inject_cleanup_exit,fail_create,worker_started,release_worker;
static atomic_uint extra_unwinds;
static atomic_bool hold_finish,inside_finish,release_finish;
static atomic_bool restore_attempted,restore_returned;
static _Thread_local bool watched_finish;
bool agent_shutdown_requested(void){return atomic_load(&shutdown_flag);}
void agent_request_shutdown(void){atomic_store(&shutdown_flag,true);}

#ifdef TEST_NUTTX_STYLE
/* Exact actual NuttX tls_cleanup.c implementation below; only TLS storage
 * access is a host thread-local substitute. Model teardown interrupting the
 * old callback after free, while pop has not removed its TLS record yet. */
#define FAR
#define DEBUGASSERT assert
#define CONFIG_TLS_NCLEANUP 4
typedef void (*tls_cleanup_t)(void *);
struct tls_cleanup_s {tls_cleanup_t tc_cleaner;void *tc_arg;};
struct tls_info_s {unsigned tl_tos;struct tls_cleanup_s tl_stack[CONFIG_TLS_NCLEANUP];};
static _Thread_local struct tls_info_s local_tls;
/* ACTUAL_NUTTX_TLS_CLEANUP */
#undef pthread_cleanup_push
#undef pthread_cleanup_pop
#define pthread_cleanup_push(fn,arg) tls_cleanup_push(&local_tls,fn,arg)
#define pthread_cleanup_pop(execute) tls_cleanup_pop(&local_tls,execute)
#endif

static void fixture_syslog(int priority,const char *format,...);
#define syslog fixture_syslog
#include LIFECYCLE_SOURCE
static void fixture_syslog(int priority,const char *format,...)
{
    (void)priority;(void)format;
#ifndef EXPECT_BROKEN
    /* Diagnostics must never execute with lifecycle lock held. */
    assert(!pthread_mutex_trylock(&lock));pthread_mutex_unlock(&lock);
#endif
}
int __real_pthread_cond_broadcast(pthread_cond_t *condition);
int __real_pthread_setcancelstate(int state,int *previous);
int __wrap_pthread_setcancelstate(int state,int *previous)
{
    bool watched=watched_finish&&state==PTHREAD_CANCEL_ENABLE;
    if(watched)atomic_store(&restore_attempted,true);
    int rc=__real_pthread_setcancelstate(state,previous);
    if(watched)atomic_store(&restore_returned,true);
    return rc;
}
int __wrap_pthread_cond_broadcast(pthread_cond_t *condition)
{
    if(condition==&changed&&atomic_load(&hold_finish)){
        int previous=PTHREAD_CANCEL_ENABLE;
        assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous));
        assert(previous==PTHREAD_CANCEL_DISABLE);
        pthread_setcancelstate(previous,NULL);
        watched_finish=true;
        atomic_store(&inside_finish,true);
        while(!atomic_load(&release_finish))usleep(1000);
    }
    return __real_pthread_cond_broadcast(condition);
}

void __real_free(void *pointer);
void __wrap_free(void *pointer)
{
#ifdef TEST_NUTTX_STYLE
    bool reenter=local_tls.tl_tos &&
        local_tls.tl_stack[local_tls.tl_tos-1].tc_cleaner==task_finished &&
        atomic_exchange(&inject_cleanup_exit,false);
#endif
    __real_free(pointer);
#ifdef TEST_NUTTX_STYLE
    if(reenter){atomic_fetch_add(&extra_unwinds,1);tls_cleanup_popall(&local_tls);}
#endif
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg)
{return atomic_load(&fail_create)?EAGAIN:__real_pthread_create(thread,attr,fn,arg);}
static void *quick(void *argument){return argument;}
#ifndef EXPECT_BROKEN
static void counts(unsigned total,unsigned network)
{
    pthread_mutex_lock(&lock);assert(workers==total&&network_workers==network);
    unsigned listed=0,listed_network=0;
    for(struct tracked_task *task=registered_tasks;task;task=task->next){listed++;listed_network+=task->network;}
    assert(listed==workers&&listed_network==network_workers);pthread_mutex_unlock(&lock);
}
static void *hold(void *argument)
{
    atomic_store(&worker_started,true);
    while(!atomic_load(&release_worker))usleep(1000);
    return argument;
}
static void wait_started(void)
{for(int i=0;i<3000&&!atomic_load(&worker_started);i++)usleep(1000);assert(atomic_load(&worker_started));}
#ifdef TEST_NUTTX_STYLE
static void *cancel_style(void *argument)
{tls_cleanup_popall(&local_tls);return argument;}
#elif !defined(TEST_SKIP_NATIVE_UNWIND)
static void *exit_style(void *argument){pthread_exit(argument);return NULL;}
static void *async_quick(void *argument)
{assert(!pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL));return argument;}
#endif
#endif
int main(void)
{
    pthread_t thread;
#ifndef EXPECT_BROKEN
    void *result;
#endif
#ifdef EXPECT_BROKEN
    atomic_store(&inject_cleanup_exit,true);
    assert(!agent_tracked_pthread_create(&thread,NULL,quick,NULL,"net_watch"));
    pthread_join(thread,NULL);
    assert(0&&"old cleanup must report ASan UAF");
#else
    for(unsigned i=0;i<200;i++){
        atomic_store(&inject_cleanup_exit,true);
        assert(!agent_tracked_pthread_create(&thread,NULL,quick,(void *)(uintptr_t)i,i%2?"outbound":"net_watch"));
        assert(!pthread_join(thread,&result)&&result==(void *)(uintptr_t)i);
        counts(0,0);
    }
    assert(!atomic_load(&extra_unwinds)); /* Normal finish now has no live TLS hook. */
    atomic_store(&inject_cleanup_exit,false);

    uintptr_t stale=worker_sequence;
    atomic_store(&worker_started,false);atomic_store(&release_worker,false);
    assert(!agent_tracked_pthread_create(&thread,NULL,hold,NULL,"net_watch"));wait_started();counts(1,1);
    for(int i=0;i<20;i++)task_finished((void *)stale);
    counts(1,1); /* stale token cannot consume a later registration */
    atomic_store(&release_worker,true);assert(!pthread_join(thread,NULL));counts(0,0);
    task_finished((void *)worker_sequence);counts(0,0);

    atomic_store(&fail_create,true);
    assert(agent_tracked_pthread_create(&thread,NULL,quick,NULL,"net_watch")==EAGAIN);
    uintptr_t failed=worker_sequence;counts(0,0);
    atomic_store(&fail_create,false);task_finished((void *)failed);counts(0,0);

    /* Pause normal cleanup while its lock is held. Cancellation stays
     * disabled from before pop(0) until lock/free/log work has completed. */
    atomic_store(&hold_finish,true);atomic_store(&inside_finish,false);atomic_store(&release_finish,false);
    atomic_store(&restore_attempted,false);atomic_store(&restore_returned,false);
#if !defined(TEST_NUTTX_STYLE) && !defined(TEST_SKIP_NATIVE_UNWIND)
    assert(!agent_tracked_pthread_create(&thread,NULL,async_quick,NULL,"net_watch"));
#else
    assert(!agent_tracked_pthread_create(&thread,NULL,quick,NULL,"net_watch"));
#endif
    for(int i=0;i<3000&&!atomic_load(&inside_finish);i++)usleep(1000);
    assert(atomic_load(&inside_finish));assert(pthread_mutex_trylock(&lock)==EBUSY);
#if !defined(TEST_NUTTX_STYLE) && !defined(TEST_SKIP_NATIVE_UNWIND)
    assert(!pthread_cancel(thread));
#endif
    atomic_store(&release_finish,true);assert(!pthread_join(thread,&result));
#if !defined(TEST_NUTTX_STYLE) && !defined(TEST_SKIP_NATIVE_UNWIND)
    /* On this host cleanup-pop can restore deferred cancellation, so an
     * already completed thread may return before another cancellation point.
     * Both outcomes require a restored state and a fully usable registry. */
    assert(atomic_load(&restore_attempted));
    assert(result==PTHREAD_CANCELED||result==NULL);
#endif
    atomic_store(&hold_finish,false);counts(0,0);

#ifdef TEST_NUTTX_STYLE
    /* The same actual NuttX pop-all reentry is safe for an exit/cancel
     * callback: only a token remains, so the second leave finds no record. */
    atomic_store(&inject_cleanup_exit,true);
    assert(!agent_tracked_pthread_create(&thread,NULL,cancel_style,NULL,"net_watch"));
    assert(!pthread_join(thread,NULL));counts(0,0);
    assert(atomic_load(&extra_unwinds)==1);
#elif !defined(TEST_SKIP_NATIVE_UNWIND)
    assert(!agent_tracked_pthread_create(&thread,NULL,exit_style,(void *)7,"net_watch"));
    assert(!pthread_join(thread,&result)&&result==(void *)7);counts(0,0);
    atomic_store(&worker_started,false);atomic_store(&release_worker,false);
    assert(!agent_tracked_pthread_create(&thread,NULL,hold,NULL,"net_watch"));wait_started();
    assert(!pthread_cancel(thread));assert(!pthread_join(thread,&result)&&result==PTHREAD_CANCELED);counts(0,0);
#endif

    /* Ownership can only be released after actual workers are gone. */
    uint64_t owner=agent_session_owner_new();assert(owner&&agent_instance_acquire_owned(owner,NULL));
    assert(agent_instance_shutdown_owned(owner));agent_tasks_wait(true);agent_tasks_wait(false);
    agent_instance_release();assert(!agent_instance_wait_owned(owner,10));
    assert(agent_tracked_pthread_create(&thread,NULL,quick,NULL,"net_watch")==ECANCELED);counts(0,0);
    atomic_store(&shutdown_flag,false);

    /* End-of-process exhaustion test: never wrap/reuse a pointer-width ID. */
    worker_sequence=UINTPTR_MAX-1;
    assert(!agent_tracked_pthread_create(&thread,NULL,quick,NULL,"net_watch"));
    assert(!pthread_join(thread,NULL));counts(0,0);assert(worker_sequence==UINTPTR_MAX);
    assert(agent_tracked_pthread_create(&thread,NULL,quick,NULL,"net_watch")==EOVERFLOW);counts(0,0);
    task_finished((void *)stale);counts(0,0);
    puts("PASS: actual lifecycle200 normal exits, stable token/stale duplicate isolation, real registration counts, create failure, exit/cancel, owner waits and nonwrapping uintptr exhaustion");
#endif
    return 0;
}

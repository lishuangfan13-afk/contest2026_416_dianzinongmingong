/* Exercise the actual detached-worker completion barrier with real threads. */
#include "infra/task_lifecycle.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static atomic_bool stopping;
static atomic_uint completed;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t released = PTHREAD_COND_INITIALIZER;
static unsigned mask;

bool agent_shutdown_requested(void) { return atomic_load(&stopping); }
void agent_request_shutdown(void) { atomic_store(&stopping,true); }

static void *worker(void *argument)
{
    unsigned bit = (unsigned)(uintptr_t)argument;
    pthread_mutex_lock(&gate);
    while (!(mask & bit)) pthread_cond_wait(&released, &gate);
    pthread_mutex_unlock(&gate);
    atomic_fetch_or(&completed, bit);
    return NULL;
}

static void release(unsigned bit)
{
    pthread_mutex_lock(&gate);
    mask |= bit;
    pthread_cond_broadcast(&released);
    pthread_mutex_unlock(&gate);
}

int main(void)
{
    for (int round = 0; round < 50; round++) {
        atomic_store(&stopping, false);
        atomic_store(&completed, 0);
        mask = 0;
        assert(agent_instance_acquire());
        assert(!agent_instance_acquire());
        pthread_t threads[3];
        const char *names[] = {"net_watch", "agent_loop", "outbound"};
        for (unsigned i = 0; i < 3; i++) {
            assert(agent_tracked_pthread_create(&threads[i], NULL, worker,
                       (void *)(uintptr_t)(1u << i), names[i]) == 0);
            assert(pthread_detach(threads[i]) == 0);
        }
        atomic_store(&stopping, true);
        pthread_t rejected;
        assert(agent_tracked_pthread_create(&rejected, NULL, worker,
                   (void *)(uintptr_t)8, "late_worker") == ECANCELED);
        release(1);
        agent_tasks_wait(true);
        assert(atomic_load(&completed) == 1); /* Other workers still own resources. */
        assert(!agent_instance_acquire());
        release(2 | 4);
        agent_tasks_wait(false);
        assert(atomic_load(&completed) == 7);
        agent_instance_release();
    }
    assert(agent_instance_acquire());
    agent_instance_release();
    puts("PASS: 50 single-instance/worker-drain cycles, network-first barrier and late-worker refusal.");
    return 0;
}

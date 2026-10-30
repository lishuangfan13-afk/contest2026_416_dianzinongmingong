/* SPDX-License-Identifier: Apache-2.0
 * Exercise the actual patched bus while external requests race shutdown.
 */
#include "core/message_bus.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *consume(void *outbound)
{
    agent_msg_t msg = {0};
    int result = outbound ? message_bus_pop_outbound(&msg, UINT32_MAX)
                          : message_bus_pop_inbound(&msg, UINT32_MAX);
    assert(result != 0);
    return NULL;
}

static void *produce(void *outbound)
{
    agent_msg_t msg = {0};
    msg.content = strdup("caller owns rejected content");
    assert(msg.content);
    int result = outbound ? message_bus_push_outbound(&msg)
                          : message_bus_push_inbound(&msg);
    assert(result != 0);
    message_bus_msg_free(&msg);
    return NULL;
}

static void run_shutdown(void *(*operation)(void *))
{
    pthread_t inbound, outbound;
    assert(pthread_create(&inbound, NULL, operation, NULL) == 0);
    assert(pthread_create(&outbound, NULL, operation, (void *)1) == 0);
    struct timespec pause = { .tv_nsec = 2000000 };
    nanosleep(&pause, NULL);
    message_bus_destroy();
    assert(pthread_join(inbound, NULL) == 0);
    assert(pthread_join(outbound, NULL) == 0);
    assert(!message_bus_is_ready());
}

int main(void)
{
    for (int round = 0; round < 20; round++) {
        assert(message_bus_init() == 0);
        run_shutdown(consume); /* Empty queues: wake blocked consumers. */
        assert(message_bus_init() == 0);
        for (int i = 0; i < 16; i++) {
            agent_msg_t a = {0}, b = {0};
            a.content = strdup("queued inbound");
            b.content = strdup("queued outbound");
            assert(a.content && b.content);
            assert(message_bus_push_inbound(&a) == 0);
            assert(message_bus_push_outbound(&b) == 0);
        }
        run_shutdown(produce); /* Full queues: reject and free correctly. */
    }
    agent_msg_t msg = {0};
    assert(message_bus_pop_inbound(&msg, 0) != 0);
    assert(message_bus_pop_outbound(&msg, 0) != 0);
    message_bus_destroy();
    return 0;
}

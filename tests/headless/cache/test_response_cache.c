/* This harness never sends a prompt or performs a network/serial operation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static time_t now = 1000;
static time_t fixture_time(time_t *out)
{
    if (out) *out = now;
    return now;
}
static void fixture_syslog(int priority, const char *format, ...)
{
    (void)priority;
    (void)format; /* Never emit prompt, answer, or cache contents. */
}
#define time fixture_time
#define syslog fixture_syslog
#define llm_cache_get actual_llm_cache_get
#define llm_cache_put actual_llm_cache_put
#define llm_cache_put_tokens actual_llm_cache_put_tokens

/* ACTUAL_CACHE_IMPLEMENTATION */

#undef llm_cache_get
#undef llm_cache_put
#undef llm_cache_put_tokens

static unsigned get_calls, put_calls, token_calls, fresh_calls, checks;
static char *llm_cache_get(const char *prompt, size_t length)
{
    get_calls++;
    return actual_llm_cache_get(prompt, length);
}
static void llm_cache_put(const char *prompt, size_t length, const char *response)
{
    put_calls++;
    actual_llm_cache_put(prompt, length, response);
}
static void llm_cache_put_tokens(const char *prompt, size_t length, int count)
{
    token_calls++;
    actual_llm_cache_put_tokens(prompt, length, count);
}

typedef struct {const char *content;} agent_msg_t;
enum {LLM_COMPLEXITY_SIMPLE, LLM_COMPLEXITY_COMPLEX, AGENT_TRACE_OK};
static void agent_trace_step(int *trace, int a, void *b, int c, int d)
{
    (void)trace; (void)a; (void)b; (void)c; (void)d;
}
static void agent_trace_end(int *trace, int status)
{
    (void)trace; (void)status;
}

#ifdef CONFIG_EXAMPLES_ZHAOXI
typedef int product_mutation_t;
enum {PRODUCT_MUTATION_NONE, PRODUCT_MUTATION_WRITE};
static int mutation_mode;
static product_mutation_t product_mutation_intent(const char *content)
{
    (void)content;
    return mutation_mode;
}
static int cJSON_GetArraySize(const void *messages)
{
    (void)messages;
    return 3;
}
#endif

/* Only the actual initialization and both actual cache guards are inserted.
 * The middle of the agent is represented by one deterministic fresh result. */
static char *policy_turn(const char *prompt, const char *fresh, int complexity,
                         bool response_ok, bool watchdog_fired,
                         int last_total_tokens)
{
    const agent_msg_t message = {prompt};
    const agent_msg_t *msg = &message;
    char *final_text = NULL;
    int trace = 0;
#ifdef CONFIG_EXAMPLES_ZHAOXI
    const void *messages = NULL;
#endif
    /* ACTUAL_CACHE_INITIALIZATION */
#ifdef CONFIG_EXAMPLES_ZHAOXI
    (void)mutation;
    (void)mutation_first;
    (void)mutation_repair_sent;
#endif
    /* ACTUAL_CACHE_LOOKUP */
    fresh_calls++;
    final_text = strdup(fresh);
    assert(final_text != NULL);
    cache_response_ok = response_ok;
send_reply:
    /* ACTUAL_CACHE_STORE */
    return final_text;
}

static void reset(void)
{
    llm_cache_init();
    get_calls = put_calls = token_calls = fresh_calls = 0;
    now = 1000;
#ifdef CONFIG_EXAMPLES_ZHAOXI
    mutation_mode = PRODUCT_MUTATION_NONE;
#endif
}

static void assert_reply(char *reply, const char *expected)
{
    assert(reply != NULL && !strcmp(reply, expected));
    free(reply);
    checks++;
}

static void legacy_reproductions(void)
{
    char first[128], second[128];
    memset(first, 'A', LLM_CACHE_KEY_LEN);
    memset(second, 'A', LLM_CACHE_KEY_LEN);
    strcpy(first + LLM_CACHE_KEY_LEN, " QATEST_SYNTHETIC_A");
    strcpy(second + LLM_CACHE_KEY_LEN, " QATEST_SYNTHETIC_B");
    assert(strcmp(first, second) && djb2_hash(first, strlen(first)) ==
                                   djb2_hash(second, strlen(second)));
    reset();
    actual_llm_cache_put(first, strlen(first), "ANSWER_A");
    assert_reply(actual_llm_cache_get(second, strlen(second)), "ANSWER_A");
    actual_llm_cache_put(second, strlen(second), "ANSWER_B");
    assert_reply(actual_llm_cache_get(first, strlen(first)), "ANSWER_B");

    /* Distinct two-byte inputs also collide, without prefix truncation. */
    assert(djb2_hash("Az", 2) == djb2_hash("BY", 2));
    reset(); actual_llm_cache_put("Az", 2, "COLLISION_A");
    assert_reply(actual_llm_cache_get("BY", 2), "COLLISION_A");

    /* The cache key has no session/context/config dimension. */
    reset(); actual_llm_cache_put("same prompt", 11, "CONTEXT_A");
    assert_reply(actual_llm_cache_get("same prompt", 11), "CONTEXT_A");
    now += LLM_CACHE_TTL_SEC + 1;
    assert(actual_llm_cache_get("same prompt", 11) == NULL); checks++;
    assert(actual_llm_cache_get(NULL, 0) == NULL); checks++;
    llm_cache_cleanup();
}

static void policy_reproductions(void)
{
    char first[128], second[128];
    memset(first, 'B', LLM_CACHE_KEY_LEN);
    memset(second, 'B', LLM_CACHE_KEY_LEN);
    strcpy(first + LLM_CACHE_KEY_LEN, " QATEST_SYNTHETIC_A");
    strcpy(second + LLM_CACHE_KEY_LEN, " QATEST_SYNTHETIC_B");
    reset();
    actual_llm_cache_put(first, strlen(first), "STALE_SEEDED_ANSWER");
    char *reply = policy_turn(second, "FRESH_ANSWER_B", LLM_COMPLEXITY_SIMPLE,
                              true, false, 7);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert_reply(reply, "FRESH_ANSWER_B");
    assert(!get_calls && !put_calls && !token_calls && fresh_calls == 1);
    assert_reply(actual_llm_cache_get(first, strlen(first)), "STALE_SEEDED_ANSWER");
#else
    assert_reply(reply, "STALE_SEEDED_ANSWER");
    assert(get_calls == 1 && !put_calls && !token_calls && !fresh_calls);
#endif

    reset();
    assert_reply(policy_turn(first, "ANSWER_A", LLM_COMPLEXITY_SIMPLE,
                             true, false, 7), "ANSWER_A");
    reply = policy_turn(second, "ANSWER_B", LLM_COMPLEXITY_SIMPLE, true, false, 7);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert_reply(reply, "ANSWER_B");
    assert(!get_calls && !put_calls && !token_calls && fresh_calls == 2);
#else
    assert_reply(reply, "ANSWER_A");
    assert(get_calls == 2 && put_calls == 1 && token_calls == 1 && fresh_calls == 1);
#endif

    reset();
    assert_reply(policy_turn("same prompt", "CONTEXT_A", LLM_COMPLEXITY_SIMPLE,
                             true, false, 7), "CONTEXT_A");
    reply = policy_turn("same prompt", "CONTEXT_B", LLM_COMPLEXITY_SIMPLE,
                        true, false, 7);
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert_reply(reply, "CONTEXT_B");
    assert(!get_calls && !put_calls && !token_calls && fresh_calls == 2);
    mutation_mode = PRODUCT_MUTATION_WRITE;
    assert_reply(policy_turn("mutation", "FRESH_MUTATION", LLM_COMPLEXITY_SIMPLE,
                             true, false, 7), "FRESH_MUTATION");
    assert(!get_calls && !put_calls && !token_calls);
#else
    assert_reply(reply, "CONTEXT_A");
    assert(get_calls == 2 && put_calls == 1 && token_calls == 1 && fresh_calls == 1);
#endif

    for (unsigned outcome = 0; outcome < 3; outcome++) {
        reset();
        bool ok = outcome != 0;
        bool watchdog = outcome == 1;
        int count = outcome == 2 ? 0 : 7;
        assert_reply(policy_turn("uncached", "FRESH", LLM_COMPLEXITY_SIMPLE,
                                 ok, watchdog, count), "FRESH");
#ifdef CONFIG_EXAMPLES_ZHAOXI
        assert(!get_calls && !put_calls && !token_calls);
#else
        assert(get_calls == 1 && put_calls == (unsigned)(ok && !watchdog) && !token_calls);
#endif
    }
    reset();
    assert_reply(policy_turn("complex", "FRESH_COMPLEX", LLM_COMPLEXITY_COMPLEX,
                             true, false, 7), "FRESH_COMPLEX");
    assert(!get_calls && !put_calls && !token_calls);
    reset();
    assert_reply(policy_turn(NULL, "FRESH_NULL", LLM_COMPLEXITY_SIMPLE,
                             true, false, 7), "FRESH_NULL");
    assert(!get_calls && !put_calls && !token_calls);
    llm_cache_cleanup();
}

int main(void)
{
    legacy_reproductions();
    policy_reproductions();
#ifdef CONFIG_EXAMPLES_ZHAOXI
    printf("RESPONSE_CACHE_PRODUCT: %u checks passed; lookup/store/token calls blocked\n", checks);
#else
    printf("RESPONSE_CACHE_GENERIC: %u checks passed; legacy collisions reproduced\n", checks);
#endif
    return 0;
}

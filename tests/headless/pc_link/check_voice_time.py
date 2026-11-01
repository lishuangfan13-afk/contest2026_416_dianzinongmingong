#!/usr/bin/env python3
"""Compile actual time formatting and fast-path routing for both SDK profiles.

time() and time-tool output are fixtures. localtime_r() uses the real host
timezone conversion, exercising CST-8, UTC and midnight without fixed offsets.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#define AGENT_CHAN_VOICE "voice"
#define AGENT_DATA_DIR "/emmc/zhaoxi"
#define AGENT_MEMORY_DIR "/emmc/zhaoxi/memory"
#define OK 0
#define LOG_INFO 6
#define syslog(...) ((void)0)
typedef struct { char *content; char channel[16]; char chat_id[64]; } agent_msg_t;
static const char *TAG = "test";
static int s_fast_path_count, s_llm_path_count, tool_calls;
static time_t fixture_now = 1793452489;
static bool fail_localtime;
static const char *diagnostic = "2026-10-31 21:14:49 CST (UTC+8), UNIX epoch: 1793452489";
time_t __wrap_time(time_t *out) { if (out) *out = fixture_now; return fixture_now; }
struct tm *__real_localtime_r(const time_t *, struct tm *);
struct tm *__wrap_localtime_r(const time_t *t, struct tm *out)
{ return fail_localtime ? NULL : __real_localtime_r(t, out); }
static int tool_registry_execute(const char *n, const char *a, char *out, size_t cap)
{ (void)a; assert(!strcmp(n, "get_current_time")); tool_calls++; snprintf(out, cap, "%s", diagnostic); return 0; }
static size_t skill_loader_build_summary(char *out, size_t size)
{ if (size) out[0] = 0; return 0; }
static char *handle_slash_note(const agent_msg_t *m) { (void)m; return NULL; }
static char *handle_slash_remind(const agent_msg_t *m) { (void)m; return NULL; }

ACTUAL_CONTAINS
ACTUAL_INTENTS_AND_TIME_HELPER
ACTUAL_NL_TIME_ROUTING
ACTUAL_SLASH_ROUTING

static void expect(const char *input, const char *channel, const char *expected, bool slash)
{
    char *reply;
    if (slash) {
        agent_msg_t msg = {.content = (char *)input};
        snprintf(msg.channel, sizeof(msg.channel), "%s", channel);
        reply = handle_slash_command(&msg);
    } else reply = handle_nl_fast_path(input, channel);
    assert(reply && !strcmp(reply, expected));
    free(reply);
}

int main(void)
{
    (void)TAG; (void)s_llm_path_count;
    assert(setenv("TZ", "CST-8", 1) == 0); tzset();
#ifdef CONFIG_EXAMPLES_ZHAOXI
    expect("现在几点了", "voice", "现在是21点14分。", false);
    expect("/time", "voice", "现在是21点14分。", true);
    assert(tool_calls == 0);
    assert(setenv("TZ", "UTC0", 1) == 0); tzset();
    expect("what time is it", "voice", "现在是13点14分。", false);
    assert(setenv("TZ", "CST-8", 1) == 0); tzset();
    fixture_now = 1793462400;
    expect("几点钟", "voice", "现在是0点00分。", false);
    fixture_now = 0;
    expect("现在几点了", "voice", "当前时间还没有校准，请先校准时间。", false);
    fixture_now = 1793452489; fail_localtime = true;
    expect("/time", "voice", "当前时间还没有校准，请先校准时间。", true);
    fail_localtime = false;
    puts("product: Chinese speech, configured timezone, midnight and invalid clock passed");
#else
    expect("现在几点了", "voice", diagnostic, false);
    expect("/time", "voice", diagnostic, true);
    puts("standard: existing voice/time-tool behavior retained");
#endif
    expect("现在几点了", "cli", diagnostic, false);
    expect("/time", "cli", diagnostic, true);
    expect("现在几点了", "feishu", diagnostic, false);
    assert(tool_calls >= 3);
    puts("CLI and other-channel diagnostic tool output preserved");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root', type=Path, required=True)
    args = parser.parse_args()
    text = (args.agent_root / 'src/core/agent_loop.c').read_text()
    start = text.index('static bool contains_any(')
    contains = text[start:text.index('/* Intent table:', start)]
    start = text.index('typedef struct {', text.index('/* Intent table:'))
    end = text.index('static char* handle_nl_fast_path(', start)
    intents = text[start:end]
    start = end
    end = text.index('    /* Skill list (special:', start)
    nl = text[start:end] + '    return NULL;\n}\n'
    start = text.index('static char* handle_slash_command(')
    slash = text[start:text.index('/* /note sub-handler */', start)]
    # Also ensure the active dispatch supplies the actual message channel.
    assert 'reply = handle_nl_fast_path(msg.content, msg.channel);' in text
    code = HARNESS.replace('ACTUAL_CONTAINS', contains).replace(
        'ACTUAL_INTENTS_AND_TIME_HELPER', intents).replace(
        'ACTUAL_NL_TIME_ROUTING', nl).replace('ACTUAL_SLASH_ROUTING', slash)
    with tempfile.TemporaryDirectory(prefix='zhaoxi-voice-time-') as temporary:
        root = Path(temporary); source = root / 'test.c'; source.write_text(code)
        for product in [True, False]:
            executable = root / ('product' if product else 'standard')
            command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                       '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            if product:
                command += ['-DCONFIG_EXAMPLES_ZHAOXI=1']
            subprocess.run(command + [str(source), '-Wl,--wrap=time',
                                     '-Wl,--wrap=localtime_r', '-o', str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    main()

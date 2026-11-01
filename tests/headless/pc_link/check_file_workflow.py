#!/usr/bin/env python3
"""Compile the actual SDK local-shortcut block for product and standard profiles.

The harness supplies tool results, not a live LLM or filesystem. It verifies
whether those results are allowed to reach the next model round. No copied
implementation of the shortcut condition is used.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HARNESS = r'''
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG_INFO 6
#define AGENT_SKILLS_DIR "/emmc/zhaoxi/skills/"
#define syslog(...) ((void)0)
static const char *TAG = "test";
typedef struct { const char *name, *input; } call_t;
typedef struct { int call_count; call_t calls[2]; } response_t;
static void llm_response_free(response_t *r) { (void)r; }

static bool reaches_next_model(const char *name, const char *input,
                               const char *output, int count)
{
    response_t resp = {.call_count = count, .calls = {{name, input}}};
    const char *tool_output = output;
    char *final_text = NULL;
    bool continued = false;
    for (int once = 0; once < 1; once++) {
ACTUAL_SHORTCUT_BLOCK
        continued = true;
    }
    (void)resp; (void)tool_output; (void)TAG; (void)llm_response_free;
    free(final_text);
    return continued;
}

int main(void)
{
    const char *tasks = "{\"path\":\"/emmc/zhaoxi/TASKS.md\"}";
    const char *skill = "{\"path\":\"/emmc/zhaoxi/skills/note-taker.md\"}";
#ifdef CONFIG_EXAMPLES_ZHAOXI
    assert(reaches_next_model("read_file", tasks, "Error: file not found", 1));
    assert(reaches_next_model("read_file", tasks, "- [ ] [1] old item", 1));
    assert(reaches_next_model("write_file", tasks, "File written", 1));
    assert(reaches_next_model("edit_file", tasks, "Error: target not found", 1));
    assert(reaches_next_model("list_dir", tasks, "TASKS.md", 1));
    puts("product: missing-file recovery, read-before-write and write confirmation continue");
#else
    assert(!reaches_next_model("read_file", tasks, "Error: file not found", 1));
    assert(!reaches_next_model("read_file", tasks, "- [ ] [1] old item", 1));
    assert(!reaches_next_model("write_file", tasks, "File written", 1));
    puts("standard: existing local shortcut behavior preserved");
#endif
    assert(reaches_next_model("read_file", skill, "# Skill", 1));
    assert(reaches_next_model("get_current_time", "{}", "today", 1));
    assert(reaches_next_model("read_file", tasks, "", 1));
    assert(reaches_next_model("read_file", tasks, "parallel result", 2));
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root', type=Path, required=True)
    parser.add_argument('--cjson-root', type=Path, required=True)
    parser.add_argument('--expect-broken', action='store_true')
    args = parser.parse_args()
    text = (args.agent_root / 'src/core/agent_loop.c').read_text()
    start = text.index('        /* Product file workflows need')
    end = text.index('        /* TASK_COMPLETE detection */', start)
    block = text[start:end]
    assert block.count('Local tool shortcut: skip LLM round') == 1
    assert block.count('#ifndef CONFIG_EXAMPLES_ZHAOXI') == 1
    if args.expect_broken:
        block = block.replace('#ifndef CONFIG_EXAMPLES_ZHAOXI\n', '')
        block = block.replace('#endif /* !CONFIG_EXAMPLES_ZHAOXI */\n', '')
    with tempfile.TemporaryDirectory(prefix='zhaoxi-file-workflow-') as temporary:
        root = Path(temporary)
        source = root / 'test.c'
        source.write_text(HARNESS.replace('ACTUAL_SHORTCUT_BLOCK', block))
        for product in ([True] if args.expect_broken else [True, False]):
            executable = root / ('product' if product else 'standard')
            command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                       '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I' + str(args.cjson_root)]
            if product:
                command += ['-DCONFIG_EXAMPLES_ZHAOXI=1']
            subprocess.run(command + [str(source), str(args.cjson_root / 'cJSON.c'),
                                     '-lm', '-o', str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            if args.expect_broken:
                if result.returncode == 0 or 'Assertion' not in result.stderr:
                    raise RuntimeError('Expected shortcut regression not reproduced:\n'
                                       + result.stdout + result.stderr)
                print('before fix: missing TASKS.md terminates before the next model round')
            elif result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            else:
                print(result.stdout.strip())


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Exercise the actual SDK registry through stop/start, with stub tool bodies.

Run inside Linux/WSL. --expect-broken removes only patch 0027 from a temporary
source copy and requires the same regression test to fail before the fix.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root', type=Path, required=True)
    parser.add_argument('--cjson-root', type=Path, required=True)
    parser.add_argument('--expect-broken', action='store_true')
    args = parser.parse_args()
    source = (args.agent_root / 'src/tools/tool_registry.c').read_text()
    if args.expect_broken:
        source = source.replace('    /* A previous Agent lifetime may have freed the cached schema. */\n'
                                '    s_tools_dirty = true;\n', '')
        source = source.replace('    s_tool_count = 0;\n    s_tools_dirty = true;\n',
                                '    s_tool_count = 0;\n')
    functions = sorted(set(re.findall(r'\b(tool_\w+_execute)\b', source)) - {'tool_registry_execute'})
    with tempfile.TemporaryDirectory(prefix='zhaoxi-registry-') as temporary:
        work = Path(temporary)
        (work / 'registry.c').write_text(source)
        declarations = ['#include <stddef.h>', '#include <stdlib.h>', '#include <stdio.h>',
                        '#define AGENT_SHELL_SECURITY 1', '#define AGENT_SHELL_SECURITY_FULL 2',
                        '#define AGENT_SHELL_SECURITY_DENY 1']
        declarations += [f'int {name}(const char *, char *, size_t);' for name in functions]
        (work / 'declarations.h').write_text('\n'.join(declarations))
        stubs = ['#include "tools/tool_guard.h"', '#include <stddef.h>',
                 'int tool_web_search_init(void) { return 0; }',
                 'tool_guard_result_t tool_guard_check(const char *n, const char *i, size_t s)'
                 ' { (void)n; (void)i; (void)s; return GUARD_ALLOW; }',
                 'void tool_guard_record_call(const char *n) { (void)n; }']
        stubs += [f'int {name}(const char *i, char *o, size_t s) '
                  '{ (void)i; (void)o; (void)s; return 0; }' for name in functions]
        (work / 'stubs.c').write_text('\n'.join(stubs))
        executable = work / 'test_registry_restart'
        command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DCONFIG_EXAMPLES_ZHAOXI=1',
                   '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-I' + str(ROOT / 'tests/headless/include'),
                   '-I' + str(args.agent_root / 'src'), '-I' + str(args.cjson_root),
                   '-I' + str(args.agent_root / 'include'),
                   '-include', str(work / 'declarations.h'), str(work / 'registry.c'),
                   str(work / 'stubs.c'), str(Path(__file__).with_name('test_registry_restart.c')),
                   str(args.cjson_root / 'cJSON.c'), '-pthread', '-lm', '-o', str(executable)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        if args.expect_broken:
            if result.returncode == 0 or 'assert' not in result.stderr.lower():
                raise RuntimeError('Old source did not reproduce the expected schema failure: '
                                   + str(result.returncode) + '\n' + result.stdout + result.stderr)
            print('before fix: reproduced missing schemas after cleanup/restart')
        else:
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Reproduce the real old cache bug and exercise actual product cache guards."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def block_after(source, marker):
    start = source.index('if (', source.index(marker))
    opened = source.index('{', start)
    depth = 1
    position = opened + 1
    while depth:
        if source[position] == '{':
            depth += 1
        elif source[position] == '}':
            depth -= 1
        position += 1
    return source[start:position]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    args = parser.parse_args()
    cache = (args.sdk / 'src/llm/llm_cache.c').read_text(encoding='utf-8')
    agent = (args.sdk / 'src/core/agent_loop.c').read_text(encoding='utf-8')
    start = agent.index('static char* run_react_loop(')
    end = agent.index('static cJSON* build_messages(', start)
    react = agent[start:end]
    # Fail closed if a new entry point appears outside the three exercised calls.
    for name in ('llm_cache_get', 'llm_cache_put', 'llm_cache_put_tokens'):
        if len(re.findall(r'\b' + name + r'\s*\(', react)) != 1:
            raise SystemExit('Unexpected number of run_react_loop cache entries: ' + name)
    init = react[react.index('bool cacheable = true;'):react.index('/* Router:')]
    lookup = block_after(react, '/* Cache check:')
    store = block_after(react, '/* Cache store:')
    template = Path(__file__).with_name('test_response_cache.c').read_text(encoding='utf-8')
    replacements = {'/* ACTUAL_CACHE_IMPLEMENTATION */': cache,
                    '/* ACTUAL_CACHE_INITIALIZATION */': init,
                    '/* ACTUAL_CACHE_LOOKUP */': lookup,
                    '/* ACTUAL_CACHE_STORE */': store}
    for marker, actual in replacements.items():
        if template.count(marker) != 1:
            raise SystemExit('Expected exactly one harness marker: ' + marker)
        template = template.replace(marker, actual)
    with tempfile.TemporaryDirectory(prefix='zhaoxi-response-cache-') as directory:
        directory = Path(directory)
        source = directory / 'test.c'
        executable = directory / 'test_response_cache'
        source.write_text(template, encoding='utf-8')
        for product in (False, True):
            flags = ['-DCONFIG_EXAMPLES_ZHAOXI=1'] if product else []
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
                            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', '-pthread',
                            '-I' + str(args.sdk / 'include'), *flags,
                            str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == '__main__':
    main()

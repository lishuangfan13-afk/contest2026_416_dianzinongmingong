#!/usr/bin/env python3
"""Apply the ordered headless SDK series to a disposable pristine source copy."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SERIES = (8, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 23, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pristine-agent', required=True, type=Path,
                        help='unmodified packages_ai_agent e65550f source')
    parser.add_argument('--patched-agent', type=Path,
                        help='optionally compare patched files to the actual build source')
    args = parser.parse_args()
    applied, touched = [], set()
    with tempfile.TemporaryDirectory(prefix='zhaoxi-patches-') as temporary:
        target = Path(temporary) / 'agent'
        shutil.copytree(args.pristine_agent, target, ignore=shutil.ignore_patterns('.git'))
        for number in SERIES:
            files = list((ROOT / 'patches').glob(f'{number:04d}-packages_ai_agent-*.patch'))
            if len(files) != 1:
                raise RuntimeError(f'Expected one SDK patch numbered {number}')
            patch = files[0]
            for action in (['--check'], []):
                result = subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', *action, str(patch)], cwd=target,
                                        text=True, capture_output=True)
                if result.returncode:
                    raise RuntimeError(patch.name + '\n' + result.stderr)
            touched.update(line[6:] for line in patch.read_text().splitlines()
                           if line.startswith('+++ b/'))
            applied.append({'file': patch.name,
                            'sha256': hashlib.sha256(patch.read_bytes()).hexdigest()})
        if args.patched_agent:
            for name in sorted(touched):
                if (target / name).read_bytes() != (args.patched_agent / name).read_bytes():
                    raise RuntimeError('Build source differs from patch series: ' + name)
        report = {'source_commit': 'e65550f18759f086d7f544edcf17d1e31223244f',
                  'sequential_apply': 'passed', 'patches': applied,
                  'build_source_comparison': 'passed' if args.patched_agent else 'not requested',
                  'files': sorted(touched)}
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

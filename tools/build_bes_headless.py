#!/usr/bin/env python3
"""Build and package the verified BES AP profile in an existing Linux/WSL workspace.

Does not download sources, apply patches, format storage or flash hardware.
See docs/BES固件构建与验收.md for the exact dependency and BSP requirements.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

REPO = Path(__file__).resolve().parents[1]
BOARD = Path('vendor/bes/boards/best1700_ep/aos_evb')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, env, cwd):
    subprocess.run(list(map(str, args)), env=env, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--output', type=Path, default=REPO / 'firmware/headless')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--stage-only', action='store_true', help='package an already completed build')
    mode.add_argument('--incremental', action='store_true', help='reuse the existing Ninja configuration; compile changed files and link')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    ws = args.workspace.resolve()
    build = (args.build_dir or ws / 'cmake_out/aos_evb_headless_v1').resolve()
    compiler = ws / 'prebuilts/gcc/linux-x86_64/arm-none-eabi/bin'
    env = os.environ.copy()
    env['PATH'] = str(Path.home() / '.local/bin') + ':' + str(compiler) + ':' + env['PATH']
    sources = json.loads((REPO / 'configs/bes_headless.sources.json').read_text())
    for archive in sources['bsp_archives']:
        path = ws / archive['path']
        if not path.is_file() or digest(path) != archive['sha256']:
            raise RuntimeError('BSP archive missing or differs from the tested ABI: ' + str(path))
    if not (ws / 'nuttx/fs/fatfs/fatfs/source/ff.c').is_file():
        raise RuntimeError('Missing pinned nuttx_fs_fatfs source; do not format /emmc to work around this.')
    if not args.stage_only:
        frontend = ws / 'packages/zhaoxi'
        if not frontend.is_symlink() or frontend.resolve() != REPO / 'app/zhaoxi':
            raise RuntimeError('Link this checkout app/zhaoxi to packages/zhaoxi before building.')
        sdk = ws / 'packages/ai_agent/src/agent_main.c'
        if 'headless_network_pause' not in sdk.read_text():
            raise RuntimeError('Apply the complete ordered SDK patch series first.')
        if args.incremental:
            if not (build / 'build.ninja').is_file():
                raise RuntimeError('Incremental build requires an existing configured Ninja directory.')
        else:
            profile = ws / BOARD / 'configs/apheadless'
            profile.mkdir(exist_ok=True)
            text = (REPO / 'configs/bes_headless.defconfig').read_text()
            text = re.sub(r'^CONFIG_APPS_DIR=.*$', f'CONFIG_APPS_DIR="{ws / "apps"}"', text, flags=re.M)
            (profile / 'defconfig').write_text(text)
            # CMake does not track changes to the nested .ap startup includes.
            for name in ('rcS', 'rc.sysinit'):
                (ws / BOARD / 'src/etc/init.d' / name).touch()
            if (build / 'CMakeCache.txt').exists() and (build / 'build.ninja').exists():
                run(['cmake', '--build', build, '--target', 'resetconfig'], env, ws)
            run(['cmake', '-B', build, '-S', ws / 'nuttx',
                 '-DBOARD_CONFIG=../' + str(BOARD / 'configs/apheadless'),
                 '-DCUSTOM_MODULE_PATH=' + str(ws / 'build/cmake'),
                 '-DEXTRA_FLAGS=-Wno-cpp -Wno-deprecated-declarations', '-GNinja'], env, ws)
        run(['cmake', '--build', build, '-j' + str(args.jobs)], env, ws)
    for directory in ('etc', 'romfs_etc'):
        startup = (build / directory / 'init.d/rcS').read_text()
        mounts = (build / directory / 'init.d/rc.sysinit').read_text()
        if 'ai_agent --headless &' not in startup or 'autoformat' in mounts:
            raise RuntimeError('Generated ROMFS has stale startup or destructive mount options: ' + directory)
    image = bytearray((build / 'nuttx_ap.bin').read_bytes())
    symbols = subprocess.check_output([str(compiler / 'arm-none-eabi-nm'), str(build / 'nuttx')], text=True)
    address = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' T pmu_open'))
    offset = address + 0x1a78 - 0x10190000
    if not 0 <= offset < len(image) - 1 or struct.unpack_from('<H', image, offset)[0] != 0xb955:
        raise RuntimeError('NTC patch instruction does not match the verified BSP; no image written.')
    if struct.unpack_from('<I', image, len(image) - 4)[0] != 0x30190000:
        raise RuntimeError('BES packaged image footer is not AP address 0x30190000.')
    struct.pack_into('<H', image, offset, 0xe011)
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output / 'nuttx_ap_headless.bin'
    target.write_bytes(image)
    manifest = {'image': target.name, 'size': len(image), 'sha256': digest(target),
                'flash_address': '0x30190000', 'ntc_patch': {'offset': offset, 'from': '0xb955', 'to': '0xe011'},
                'startup_in_packaged_romfs_verified': True, 'persistent_data_root': '/emmc/zhaoxi',
                'credentials_source': 'runtime device config, not build options',
                'hardware_functional_acceptance_complete': False,
                'app_sources': {str(p.relative_to(REPO)): digest(p)
                                for p in sorted((REPO / 'app/zhaoxi').rglob('*'))
                                if p.is_file() and '__pycache__' not in p.parts
                                and (p.suffix in ('.c', '.h', '.cpp', '.hpp', '.md', '.cmake', '.json')
                                     or p.name in ('CMakeLists.txt', 'Makefile', 'Make.defs', 'Kconfig'))},
                'patches': {p.name: digest(p) for p in sorted((REPO / 'patches').glob('*.patch')) if int(p.name[:4]) >= 8}}
    target.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'image': str(target), 'sha256': manifest['sha256'], 'size': len(image)}))


if __name__ == '__main__':
    main()

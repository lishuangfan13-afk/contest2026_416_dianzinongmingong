#!/usr/bin/env python3
"""Create a credential-free, deterministic BES delivery snapshot; no board I/O."""
import argparse
import ast
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import struct
import subprocess
import urllib.parse
import zipfile

REPO = Path(__file__).resolve().parents[1]
ASSETS = ('SOUL.md', 'USER.md', 'MEMORY.md', 'cron.json', 'note-taker.md', 'reminder.md')

VERIFY = r'''"""Verify this delivery snapshot without serial, WSL or network access."""
from pathlib import Path, PurePosixPath
import ast, hashlib, json, struct
root=Path(__file__).resolve().parent
entries=[]
for line in (root/'SHA256SUMS').read_text(encoding='utf-8').splitlines():
    digest,name=line.split('  ',1);p=PurePosixPath(name)
    assert len(digest)==64 and not p.is_absolute() and '..' not in p.parts, 'Invalid checksum entry'
    assert name not in entries, 'Duplicate checksum entry'
    entries.append(name)
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest, 'Checksum mismatch: '+name
manifest=json.loads((root/'firmware/headless_pc_link/nuttx_ap_headless.manifest.json').read_text())
image=(root/'firmware/headless_pc_link/nuttx_ap_headless.bin').read_bytes()
assert len(image)==manifest['size'] and hashlib.sha256(image).hexdigest()==manifest['sha256']
assert struct.unpack_from('<I',image,len(image)-4)[0]==0x30190000
assert json.loads((root/'assets/agent_deploy/cron.json').read_text())['jobs']==[]
assert (root/'assets/agent_deploy/MEMORY.md').read_text(encoding='utf-8').startswith('# 朝夕项目记忆')
for name in entries:
    if name.endswith('.py'):ast.parse((root/name).read_text(encoding='utf-8-sig'),filename=name)
print('PASS: '+str(len(entries))+' file checksums; AP size/hash/address; clean asset templates; Python syntax.')
print('No serial port, microphone or network was opened. Run launch.py --check-only separately to inspect the host environment.')
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def secret_scan(files, source):
    if source is None:
        raise RuntimeError('Provide --secret-config for the private-value exclusion check; it is never copied.')
    config = json.loads(source.read_text(encoding='utf-8'))
    values = [v.encode() for k, v in config.items() if isinstance(v, str) and len(v) >= 6 and
              any(t in k.lower() for t in ('key', 'pass', 'token', 'ssid'))]
    alphabet = b'0123456789'
    for name, data in files.items():
        for value in values:
            at = data.find(value)
            while at >= 0:
                generic_decimal_alphabet = value == alphabet[1:] and at > 0 and data[at-1:at+len(value)] == alphabet
                if not generic_decimal_alphabet:
                    raise RuntimeError('Private configured value found in '+name+'; no value printed.')
                at = data.find(value, at+1)
    return len(values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--secret-config', type=Path, required=True)
    parser.add_argument('--date', default='2026-11-01')
    parser.add_argument('--label', default='preview')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}', args.label):
        parser.error('--label must contain only letters, digits, underscore or hyphen')
    year, month, day = map(int, args.date.split('-'))
    source_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
    files = {}
    names = ['firmware/headless_pc_link/nuttx_ap_headless.bin',
             'firmware/headless_pc_link/nuttx_ap_headless.manifest.json',
             'tools/deploy_assets.ps1', 'tools/asset_profile.ps1',
             'tools/prepare_delivery_bundle.py',
             'docs/交付核对清单.md', 'docs/外壳交接要点.md',
             'docs/BES固件构建与验收.md', 'docs/语音响应优化.md',
             'docs/bringup_logs/20261101_voice_latency.json',
             'assets/agent_deploy/README.md']
    names += ['assets/agent_deploy/'+name for name in ASSETS]
    names += [p.relative_to(REPO).as_posix() for p in sorted((REPO/'tools/pc_link').iterdir())
              if p.is_file() and p.suffix in ('.py', '.md', '.cmd')]
    for name in names:
        files[name] = (REPO/name).read_bytes()
    manifest = json.loads(files['firmware/headless_pc_link/nuttx_ap_headless.manifest.json'])
    image = files['firmware/headless_pc_link/nuttx_ap_headless.bin']
    if len(image) != manifest['size'] or digest(image) != manifest['sha256']:
        raise RuntimeError('Firmware and manifest do not match; snapshot not built.')
    if struct.unpack_from('<I', image, len(image)-4)[0] != 0x30190000:
        raise RuntimeError('Unexpected AP address footer')
    if json.loads(files['assets/agent_deploy/cron.json'])['jobs'] != []:
        raise RuntimeError('Refusing a cron template with actual jobs')

    # Keep copied scripts byte-exact. Missing documentation links open the
    # pinned public source instead of pointing at absent build dependencies.
    transformed = []
    public_source = 'https://github.com/lishuangfan13-afk/contest2026_416_dianzinongmingong/blob/'+source_commit+'/'
    for name, data in list(files.items()):
        if not name.endswith('.md'):
            continue
        def link(match):
            target = match.group(1)
            if target.startswith(('#', '/')) or re.match(r'^[A-Za-z][A-Za-z0-9+.-]*:', target):
                return match.group()
            path, separator, anchor = target.partition('#')
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), path))
            if resolved in files:
                return match.group()
            return ']('+public_source+urllib.parse.quote(resolved, safe='/')+(separator+anchor if separator else '')+')'
        changed = re.sub(r'\]\(([^\s()]+)\)', link, data.decode('utf-8'))
        if changed.encode('utf-8') != data:
            files[name] = changed.encode('utf-8'); transformed.append(name)

    files['verify_bundle.py'] = VERIFY.encode('utf-8')
    files['README.md'] = (f'''# 朝夕 BES 交付快照（{args.label}）

先读 [交付核对清单](docs/交付核对清单.md)。这是当前工作文件的快照，不是完整离线开发环境，也没有自动烧录功能。

- 基础 Git 提交：`{source_commit}`；文件校验以 `SHA256SUMS` 为准，`BUNDLE.json` 单列与该提交不同的工作文件。
- 固件：{manifest['size']:,} 字节，SHA256 `{manifest['sha256']}`，AP 地址 `{manifest['flash_address']}`。
- 校验：`python verify_bundle.py`；只检查环境：`python tools/pc_link/launch.py --check-only`。
- 日常先用 `tools/pc_link/仅共享网络.cmd`。只有在场人员主动选择 `开始自动语音.cmd` 才请求开启麦克风；结束用 `停止朝夕语音.cmd`。
- 板内服务凭据、Wi-Fi 密码、用户备忘和真实提醒没有复制进包。六项资产是仓库干净模板，不应手工覆盖板内用户文件。
- 使用环境需要 Windows、FTDI 驱动、Python/pyserial，以及已准备好的 Ubuntu-22.04 WSL root、pppd/ip/iptables、PPP/TUN 设备。当前实测版本为 Python3.14.3 与 pyserial3.5；包内不含这些安装依赖。
- 独立 Wi-Fi 和最新真人三功能结果由主推更新，本快照不将它们提前记为通过。固件和主机脚本应由主推同步刷新后交付。

本目录所有原始串口、录音、运行态通道令牌及实际设备配置均应保留在操作者本机，不作为公开材料。运行后产生的 `out/pc_link` 不属于这份原始交付快照。
''').encode('utf-8')
    changed_sources = []
    for name in names:
        original = subprocess.run(['git', 'show', source_commit+':'+name], cwd=REPO, capture_output=True)
        if original.returncode or original.stdout != (REPO/name).read_bytes():
            changed_sources.append(name)
    metadata = {'format': 1, 'delivery_date': args.date, 'label': args.label,
                'base_git_commit': source_commit, 'snapshot_uses_working_files': True,
                'source_files_byte_different_from_git_blob': sorted(changed_sources),
                'docs_links_rewritten_to_pinned_source': transformed,
                'firmware_sha256': manifest['sha256'], 'firmware_size': manifest['size'],
                'hardware_acceptance_complete': False, 'private_device_data_included': False,
                'source_file_hashes': {name: digest(data) for name, data in sorted(files.items())}}
    files['BUNDLE.json'] = (json.dumps(metadata, ensure_ascii=False, indent=2)+'\n').encode('utf-8')
    files['SHA256SUMS'] = ''.join(digest(data)+'  '+name+'\n' for name, data in sorted(files.items())).encode('utf-8')
    scanned = secret_scan(files, args.secret_config)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    bundle_name = 'BES_zhaoxi_'+args.date.replace('-', '')+'_'+source_commit[:7]+'_'+args.label
    folder = output/bundle_name
    folder.mkdir(exist_ok=True)
    unexpected = {p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file()}-files.keys()
    if unexpected:
        raise RuntimeError('Choose a new label; existing output has unexpected files')
    for name, data in files.items():
        target = folder/name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    subprocess.run(['python', str(folder/'verify_bundle.py')], check=True)
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for name, data in sorted(files.items()):
            entry = zipfile.ZipInfo(bundle_name+'/'+name, date_time=(year, month, day, 0, 0, 0))
            entry.create_system = 3; entry.external_attr = 0o100644 << 16; entry.compress_type = zipfile.ZIP_DEFLATED
            zipped.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    payload = archive.getvalue(); zip_path = output/(bundle_name+'.zip')
    zip_path.write_bytes(payload)
    zip_path.with_suffix('.zip.sha256').write_text(digest(payload)+'  '+zip_path.name+'\n', encoding='utf-8')
    with zipfile.ZipFile(zip_path) as zipped:
        if zipped.testzip():
            raise RuntimeError('ZIP integrity check failed')
    result = {'directory': str(folder), 'zip': str(zip_path), 'zip_sha256': digest(payload),
              'files': len(files), 'private_values_checked': scanned, 'secret_matches': 0,
              'zip_integrity': 'passed', 'firmware_manifest': 'passed', 'hardware_operations': False}
    (output/(bundle_name+'_receipt.json')).write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()

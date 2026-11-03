#!/usr/bin/env python3
"""Create a credential-free, deterministic BES delivery snapshot; no board I/O."""
import argparse
from datetime import date
import ast
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import struct
import sys
import subprocess
import urllib.parse
import zipfile

REPO = Path(__file__).resolve().parents[1]
ASSETS = ('SOUL.md', 'USER.md', 'MEMORY.md', 'cron.json', 'note-taker.md', 'reminder.md')
ESP_IMAGE = 'firmware/easymatrix/EasyMatrix.ino.bin'
ESP_MANIFEST = 'firmware/easymatrix/EasyMatrix.ino.manifest.json'
EXTENSION_SUFFIXES = {'.ino', '.h', '.hpp', '.c', '.cpp', '.json', '.csv', '.md', '.png'}
EXTENSION_EXCLUDED_DIRS = {'.git', '__pycache__', 'build', 'out', 'cache', '.pio'}

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
bundle=json.loads((root/'BUNDLE.json').read_text(encoding='utf-8'))
assert bundle['firmware_sha256']==manifest['sha256'] and bundle['firmware_size']==len(image)
esp_manifest_path=root/'firmware/easymatrix/EasyMatrix.ino.manifest.json'
esp_image_path=root/'firmware/easymatrix/EasyMatrix.ino.bin'
assert esp_manifest_path.exists()==esp_image_path.exists(), 'Incomplete EasyMatrix release'
if esp_manifest_path.exists():
    esp=json.loads(esp_manifest_path.read_text(encoding='utf-8'))
    app=esp_image_path.read_bytes()
    assert esp['image']=='EasyMatrix.ino.bin' and esp['flash_address']=='0x10000'
    assert len(app)==esp['size'] and hashlib.sha256(app).hexdigest()==esp['sha256']
    assert len(app)!=4*1024*1024, 'A full Flash backup is not a release app'
    assert bundle['components']['easymatrix']['sha256']==esp['sha256']
assert json.loads((root/'assets/agent_deploy/cron.json').read_text())['jobs']==[]
assert (root/'assets/agent_deploy/MEMORY.md').read_text(encoding='utf-8').startswith('# 朝夕项目记忆')
for name in entries:
    if name.endswith('.py'):ast.parse((root/name).read_text(encoding='utf-8-sig'),filename=name)
print('PASS: '+str(len(entries))+' file checksums; release firmware and manifests; clean assets; Python syntax.')
print('Ready for host preflight: python tools/pc_link/launch.py --check-only')
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def secret_scan(files, source):
    if source is None:
        raise RuntimeError('Provide --secret-config for the private-value exclusion check; it is never copied.')
    config = json.loads(source.read_text(encoding='utf-8'))
    values = set()
    def collect(value, key=''):
        if isinstance(value, dict):
            for child_key, child in value.items():
                collect(child, key+'.'+str(child_key))
        elif isinstance(value, list):
            for child in value:
                collect(child, key)
        elif isinstance(value, str) and len(value) >= 6 and any(
                token in key.lower() for token in ('key', 'pass', 'token', 'ssid', 'secret', 'receive_id')):
            values.add(value.encode())
    collect(config)
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



def extension_source_names():
    """Copy rebuildable EasyMatrix sources/resources, excluding generated output."""
    root = REPO / 'extension/EasyMatrix'
    if not root.is_dir():
        return []
    names = []
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part in EXTENSION_EXCLUDED_DIRS or part.startswith('.') for part in relative.parts):
            continue
        if path.is_symlink():
            raise RuntimeError('Extension source symlink is outside the delivery allowlist: '+str(relative))
        if path.is_file() and path.suffix.lower() in EXTENSION_SUFFIXES:
            names.append(path.relative_to(REPO).as_posix())
    return names


def release_manifest(files, image_name, manifest_name, address):
    """Validate an explicitly selected application/manifest pair."""
    manifest = json.loads(files[manifest_name])
    image = files[image_name]
    if manifest.get('image') != Path(image_name).name:
        raise RuntimeError('Release manifest image name differs: '+manifest_name)
    if manifest.get('flash_address') != address:
        raise RuntimeError('Unexpected release flash address: '+manifest_name)
    if isinstance(manifest.get('size'), bool) or manifest.get('size') != len(image):
        raise RuntimeError('Release image size differs: '+image_name)
    if manifest.get('sha256') != digest(image):
        raise RuntimeError('Release image checksum differs: '+image_name)
    return manifest


def add_optional_display_release(files, names):
    # Only this app/manifest pair is public. Never glob binary files from
    # extension or firmware: merged images and original Flash dumps stay out.
    image = REPO / ESP_IMAGE
    manifest = REPO / ESP_MANIFEST
    if image.exists() != manifest.exists():
        raise RuntimeError('EasyMatrix release needs both app and manifest before packaging')
    if not image.exists():
        return None
    for name in (ESP_IMAGE, ESP_MANIFEST):
        path = REPO/name
        if path.is_symlink():
            raise RuntimeError('Release files must be regular files: '+name)
        names.append(name)
        files[name] = path.read_bytes()
    release = release_manifest(files, ESP_IMAGE, ESP_MANIFEST, '0x10000')
    if len(files[ESP_IMAGE]) == 4*1024*1024:
        raise RuntimeError('An original 4 MiB Flash backup is not a release app')
    for name, expected in release.get('source_hashes', {}).items():
        source = PurePosixPath(name)
        if source.is_absolute() or '..' in source.parts:
            raise RuntimeError('Invalid display source path in release manifest')
        selected = source.as_posix() if source.parts[:1] == ('extension',) else 'extension/EasyMatrix/'+source.as_posix()
        if selected in files and digest(files[selected]) != expected:
            raise RuntimeError('Display release/source mismatch: '+selected)
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--secret-config', type=Path, required=True)
    parser.add_argument('--date', default=date.today().isoformat())
    parser.add_argument('--label', default='preview')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}', args.label):
        parser.error('--label must contain only letters, digits, underscore or hyphen')
    try:
        selected_date = date.fromisoformat(args.date)
    except ValueError:
        parser.error('--date must be a valid YYYY-MM-DD date')
    if not 1980 <= selected_date.year <= 2107:
        parser.error('--date must be representable in ZIP timestamps (1980..2107)')
    year, month, day = selected_date.year, selected_date.month, selected_date.day
    source_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
    files = {}
    names = ['firmware/headless_pc_link/nuttx_ap_headless.bin',
             'firmware/headless_pc_link/nuttx_ap_headless.manifest.json',
             'tools/deploy_assets.ps1', 'tools/asset_profile.ps1',
             'tools/prepare_delivery_bundle.py',
             'docs/交付核对清单.md', 'docs/外壳交接要点.md',
             'docs/20261102夜间工作交接.md',
             'docs/项目完整交接.md',
             'docs/三项核心功能实板验收.md', 'docs/答辩与视频准备.md',
             'docs/BES固件构建与验收.md', 'docs/无屏核心功能.md',
             'docs/语音响应优化.md', 'docs/颜文字屏使用.md',
             'docs/语音交互收尾.md',
             'docs/喝水提醒.md', 'docs/ASR唤醒短语.md',
             'docs/飞书通知接口.md', 'docs/联网方式配置.md',
             'docs/本地中文指令.md', 'docs/本地记事.md',
             'tools/display_link.py', 'assets/agent_deploy/README.md']
    names += ['assets/agent_deploy/'+name for name in ASSETS]
    names += ['tools/configure_uplink.py', 'tools/local_prompts.py',
              'tools/morning_feed.py', 'tools/export_morning_feed.py',
              'tools/publish_morning_feed.py', '.github/workflows/morning-feed.yml',
              'docs/本地语音与独立资讯.md']
    names += ['assets/local_prompts/'+name for name in (
        'manifest.json', 'hello.zxp', 'news_ready.zxp',
        'network_error.zxp', 'network_connected.zxp')]
    names += [p.relative_to(REPO).as_posix() for p in sorted((REPO/'tools/pc_link').iterdir())
              if p.is_file() and p.suffix in ('.py', '.md', '.cmd')]
    names += extension_source_names()
    # Keep reviewed contest materials with the portable handoff when present.
    # The explicit names exclude render workspaces and local review records.
    names += [name for name in (
        'docs/submission/README.md',
        'docs/submission/朝夕作品介绍.docx',
        'docs/submission/朝夕作品介绍.pdf',
        'docs/submission/朝夕架构示意.svg',
        'docs/submission/朝夕架构示意.png') if (REPO / name).is_file()]
    names = list(dict.fromkeys(names))
    for name in names:
        path = REPO/name
        if path.is_symlink():
            raise RuntimeError('Snapshot files must be regular files: '+name)
        files[name] = path.read_bytes()
    display_release = add_optional_display_release(files, names)
    manifest = release_manifest(files, 'firmware/headless_pc_link/nuttx_ap_headless.bin',
                                'firmware/headless_pc_link/nuttx_ap_headless.manifest.json',
                                '0x30190000')
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
    public_source = 'https://github.com/open-vela/contest2026_416_dianzinongmingong/blob/'+source_commit+'/'
    for name, data in list(files.items()):
        if not name.endswith('.md'):
            continue
        def link(match):
            target = match.group(1)
            if target.startswith(('#', '/')) or re.match(r'^[A-Za-z][A-Za-z0-9+.-]*:', target):
                return match.group()
            path, separator, anchor = target.partition('#')
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), urllib.parse.unquote(path)))
            if resolved in files:
                return match.group()
            return ']('+public_source+urllib.parse.quote(resolved, safe='/')+(separator+anchor if separator else '')+')'
        changed = re.sub(r'\]\(([^\s()]+)\)', link, data.decode('utf-8'))
        if changed.encode('utf-8') != data:
            files[name] = changed.encode('utf-8'); transformed.append(name)

    files['verify_bundle.py'] = VERIFY.encode('utf-8')
    display_line = (f"- EasyMatrix 应用：{display_release['size']:,} 字节，SHA256 `{display_release['sha256']}`，写入地址 `{display_release['flash_address']}`。\n"
                    if display_release else '- 本快照包含 EasyMatrix 源码与主机桥，屏端发布应用按对应固件目录准备。\n')
    files['README.md'] = (f'''# 朝夕作品交付包（{args.date} · {args.label}）

本包用于接手朝夕的语音问答、长期人设、资讯、备忘待办和提醒，包含 BES AP、手机热点配置与电脑供网工具、固定提示音、干净业务资产、扩展源码及文档。先读 [项目完整交接](docs/项目完整交接.md)，新功能的当前状态见 [本地语音与独立资讯](docs/本地语音与独立资讯.md)，早期修复见 [工作记录](docs/20261102夜间工作交接.md)。

## 版本

- 基础 Git 提交：`{source_commit}`，工作文件与提交的差异列在 `BUNDLE.json`。
- BES AP：{manifest['size']:,} 字节，SHA256 `{manifest['sha256']}`，写入地址 `{manifest['flash_address']}`。
{display_line}
## 启动

1. 先运行 `python verify_bundle.py` 核对文件和固件。
2. 保存热点与 `uplink_mode=wifi`、部署固定提示音并配置服务凭据后，开机自动联网播报“联网成功”，等待“你好，夕夕”。本板已验证 5GHz / WPA2；首次配置与音频安装见 [新功能说明](docs/本地语音与独立资讯.md)。
3. 若使用电脑 PPP，准备 Python 3、pyserial、FTDI 驱动和 Ubuntu-22.04 WSL；WSL 使用 root，装好 pppd、ip、iptables 并启用 PPP/TUN。
4. 电脑模式运行 `python tools/pc_link/launch.py --check-only`，再启动 `tools/pc_link/仅共享网络.cmd`。保留窗口，在另一终端用 `tools/pc_link/control.py` 体验业务；结束用 `停止朝夕语音.cmd`。

## 本地业务与扩展

[本地中文指令](docs/本地中文指令.md)把明确的记事、待办和相对提醒直接交给板端处理；[本地记事](docs/本地记事.md)介绍追加与回读方式。

[喝水提醒](docs/喝水提醒.md)使用本地周期任务；[唤醒短语](docs/ASR唤醒短语.md)与自动语音开关配合；[飞书通知](docs/飞书通知接口.md)配置固定收件目标后由使用者发送；[颜文字屏](docs/颜文字屏使用.md)通过独立 CH343 串口显示状态。联网模式见 [配置说明](docs/联网方式配置.md)。

API 凭据与真实记录保存在板子和操作者本机，包内业务资产是可复用的干净模板。运行后产生的 `out/pc_link` 留在本机。首次安装、外壳接线与源码重建分别见交付清单和构建说明。热点语音、本地提示音与语音人设已实板验证，公开资讯服务已在个人 fork 上线；真实早七点无人值守验收仍待观察。
''').encode('utf-8')
    changed_sources = []
    for name in names:
        original = subprocess.run(['git', 'show', source_commit+':'+name], cwd=REPO, capture_output=True)
        if original.returncode or original.stdout != (REPO/name).read_bytes():
            changed_sources.append(name)
    components = {'bes_ap': {'image': manifest['image'], 'size': manifest['size'],
                             'sha256': manifest['sha256'], 'flash_address': manifest['flash_address']}}
    if display_release:
        components['easymatrix'] = {key: display_release[key] for key in
                                   ('image', 'size', 'sha256', 'flash_address')}
    metadata = {'format': 2, 'delivery_date': args.date, 'label': args.label,
                'base_git_commit': source_commit, 'snapshot_uses_working_files': True,
                'source_files_byte_different_from_git_blob': sorted(changed_sources),
                'docs_links_rewritten_to_pinned_source': transformed,
                'firmware_sha256': manifest['sha256'], 'firmware_size': manifest['size'],
                'components': components,
                'hardware_acceptance_complete': manifest.get('hardware_functional_acceptance_complete'),
                'private_device_data_included': False,
                'original_flash_backup_included': False,
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
    subprocess.run([sys.executable, str(folder/'verify_bundle.py')], check=True)
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

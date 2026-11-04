"""Export one explicitly selected Codex visible thread in contest event format.

This is a curated historical export, not an automatic hook collector.
Input contains only user-visible messages from the desktop read_thread API.
No global configuration, network access, raw tool payloads or source edits.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import hashlib
import json
import re
from collections import Counter


def export(source, repo, session, login, team):
    raw = source.read_bytes()
    data = json.loads(raw)
    if data.get('thread_id') != session or data.get('retrieval', {}).get('hasMore'):
        raise ValueError('Expected exactly the selected, fully paginated visible thread')
    turns = sorted(data['turns'], key=lambda t: (t.get('startedAt') or 0, t['id']))
    key = re.compile(r'(?i)\b(?:sk-|tp-|ghp_|github_pat_|tvly-)[A-Za-z0-9_-]{12,}|\bAIza[A-Za-z0-9_-]{25,}|Bearer\s+[A-Za-z0-9._/+\-=]{12,}')
    secret_assignment = re.compile(r'(?i)(?:api[ _-]?key|password|密码|口令)\s*(?:[:：=]|是|为)\s*["“\']?([^\s"”\',，。；;]{6,})')
    email = re.compile(r'[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}')
    mac = re.compile(r'(?i)\b(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}\b')
    phone = re.compile(r'(?<!\d)(?:\+?86[- ]?)?1[3-9]\d{9}(?!\d)')
    account_path = re.compile(r'(?i)([A-Z]:[\\/]+Users[\\/]+)[^\\/\s<>"\']+')
    meta = re.compile(r'hook|如何导出|采集要求|原生历史|采集成功|补录|coding\s*日志|这段对话之前的历史|把.*对话.*归档', re.I)
    stats = Counter(); events = []; seen = set()
    known_secrets = set()
    for turn in turns:
        for item in turn['items']:
            text = item.get('text', '')
            known_secrets.update(key.findall(text))
            known_secrets.update(m.group(1) for m in secret_assignment.finditer(text) if re.search(r'[A-Za-z0-9]', m.group(1)))
    # Wi-Fi identifiers are learned from explicit configuration statements.
    ssids = set()
    ssid_pattern = re.compile(r'(?i)(?:Wi[- ]?Fi(?:名称)?|热点(?:名称)?|SSID)\s*(?:就叫|叫|是|为|[:：=])\s*["“\']?([A-Za-z0-9_.-]{2,64})')
    for turn in turns:
        for item in turn['items']:
            if item['role'] == 'user': ssids.update(ssid_pattern.findall(item['text']))
    for turn in turns:
        user_text = '\n'.join(i['text'] for i in turn['items'] if i['role'] == 'user')
        if (turn.get('startedAt') or 0) >= 1793805000 and meta.search(user_text):
            stats['omitted_export_discussion_turns'] += 1
            continue
        ts = datetime.fromtimestamp(turn['startedAt'], timezone.utc).isoformat().replace('+00:00', 'Z')
        for item in turn['items']:
            if item['id'] in seen: continue
            seen.add(item['id']); text = item['text']
            if key.search(text) or secret_assignment.search(text):
                stats['omitted_sensitive_messages'] += 1
                continue
            if text.lstrip().startswith(('<environment_context>', '<permissions instructions>', '<skills_instructions>')):
                continue
            substitutions = 0
            for secret in sorted(known_secrets, key=len, reverse=True):
                if secret in text:
                    substitutions += text.count(secret); text = text.replace(secret, '[REDACTED_CREDENTIAL]')
            for pattern, replacement in [(email, '[REDACTED_EMAIL]'), (mac, '[REDACTED_MAC]'), (phone, '[REDACTED_PHONE]'), (account_path, r'\1[LOCAL_USER]')]:
                text, n = pattern.subn(replacement, text); substitutions += n
            for ssid in sorted(ssids, key=len, reverse=True):
                text, n = re.subn(r'(?<![A-Za-z0-9])' + re.escape(ssid) + r'(?![A-Za-z0-9])', '[REDACTED_SSID]', text, flags=re.I)
                substitutions += n
            text, n = re.subn(r'(?i)([?&](?:key|token|secret|authorization)=)[^&\s)]+', r'\1[REDACTED]', text); substitutions += n
            text, n = re.subn(r'\b(?:oc_|ou_|cli_)[A-Za-z0-9]{12,}\b', '[REDACTED_SERVICE_ID]', text); substitutions += n
            events.append({'schema_version': '1.0', 'session_id': session, 'team_id': team,
                           'github_login': login, 'tool': 'codex', 'seq': len(events),
                           'ts': ts, 'role': item['role'], 'text': text,
                           'redacted_count': substitutions,
                           'metadata': {'export_mode': 'manual-scoped-redacted', 'source_message_id': item['id'],
                                        'timestamp_basis': 'turn_started_at', 'phase': item.get('phase', 'user')}})
            stats['redaction_substitutions'] += substitutions
    if not events: raise ValueError('No publishable visible messages')
    rel = f'logs/{login}/{events[0]["ts"][:10]}/codex__{session}.jsonl'
    target = repo / rel; target.parent.mkdir(parents=True, exist_ok=True)
    content = ''.join(json.dumps(e, ensure_ascii=False) + '\n' for e in events)
    public_text = '\n'.join(e['text'] for e in events)
    if key.search(public_text) or email.search(public_text) or mac.search(public_text) or phone.search(public_text):
        raise ValueError('Post-export sensitive-pattern check failed')
    target.write_text(content, encoding='utf-8', newline='\n')
    manifest_path = repo / 'logs' / login / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8')) if manifest_path.exists() else {'schema_version': '1.0', 'team_id': team, 'github_login': login, 'sessions': []}
    entries = [e for e in manifest['sessions'] if not (e['session_id'] == session and e['tool'] == 'codex')]
    entries.append({'session_id': session, 'tool': 'codex', 'started_at': events[0]['ts'], 'last_event_at': events[-1]['ts'],
                    'event_count': len(events), 'file_path': rel,
                    'collection_mode': 'backfill-sqlite',
                    'export_mode': 'manual-scoped-redacted', 'source_format': 'codex-desktop-visible-thread',
                    'source_cwd': r'C:\Users\[LOCAL_USER]\Documents\Codex\2026-10-22\new-chat',
                    'timestamp_basis': 'turn_started_at', 'health': 'degraded',
                    'data_completeness_warning': 'Visible development-stage conversation archive through task completion. Timestamps identify turn start. collection_mode retains the official Codex backfill compatibility value; actual source is recorded in source_format.',
                    'redacted_count_total': stats['redaction_substitutions'],
                    'source_integrity': {'main_sha256': hashlib.sha256(raw).hexdigest(), 'main_size': len(raw)},
                    'export_sha256': hashlib.sha256(content.encode('utf-8')).hexdigest()})
    manifest['sessions'] = entries
    manifest['updated_at'] = datetime.now(timezone.utc).isoformat()
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    note = repo / 'logs' / login / 'MANUAL_CODEX_EXPORT.md'
    note.write_text(f'''# Codex 本会话开发记录

本记录整理自桌面端会话历史，覆盖本会话的开发阶段，归档截至任务完成。文件使用赛事 JSONL 事件格式，与既有日志在同一账号目录管理。

- 来源：Codex 桌面端本会话，归档 {len(events)} 条可见消息。
- 采集方式：手动历史补录，`source_format=codex-desktop-visible-thread`。
- 时间戳：对应轮次开始时间，消息按会话顺序排列；同一轮的消息共享轮次时间。
- 原始工作目录保留为本会话的实际目录形式，未伪装为 openvela 工作区。
- manifest 的 `collection_mode=backfill-sqlite` 沿用官方 Codex 补录适配器当前使用的兼容字段值；实际来源由 `source_format=codex-desktop-visible-thread` 和 `export_mode` 说明，并非 SQLite。
- 导出脚本：[定向历史导出](../../tools/export_scoped_codex_log.py)。原始输入仅留本机。
- 记录：[{target.name}]({target.parent.name}/{target.name})。

对话包含开发过程、技术判断、失败尝试、用户反馈和修复结果。清单记录时间范围、消息数量与文件校验值。
''', encoding='utf-8', newline='\n')
    print(json.dumps({'exported_messages': len(events), 'read_turns': len(turns), 'file': rel, **stats}, ensure_ascii=False))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for key in ['source', 'repo', 'session', 'login', 'team']: p.add_argument('--' + key, required=True)
    a = p.parse_args()
    export(Path(a.source), Path(a.repo), a.session, a.login, a.team)

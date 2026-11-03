#!/usr/bin/env python3
"""Bounded public RSS adapter. No persona, model key or model inference here."""
import argparse
import errno
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
import hashlib
from html.parser import HTMLParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
import threading
import time
from urllib.parse import urlsplit, urlunsplit, parse_qsl, urlencode
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET

FEEDS = (
    ('Hugging Face', 'https://huggingface.co/blog/feed.xml'),
    ('Espressif', 'https://developer.espressif.com/blog/index.xml'),
    ('Raspberry Pi', 'https://www.raspberrypi.com/news/feed/'),
    ('Hackaday', 'https://hackaday.com/feed/'),
    ('Adafruit', 'https://blog.adafruit.com/feed/'),
    ('CNX Software', 'https://www.cnx-software.com/feed/'),
    ('Electronics Weekly', 'https://www.electronicsweekly.com/feed/'),
    ('Seeed Studio', 'https://www.seeedstudio.com/blog/feed/'),
)
MAX_XML = 2 * 1024 * 1024
ALIASES = {
    'ai': ('ai', 'llm', 'machine learning', 'language model', 'transformer', '模型'),
    '电子': ('electronics', 'hardware', 'embedded', 'microcontroller', 'esp32', 'raspberry', '芯片'),
    '嵌入式': ('embedded', 'firmware', 'microcontroller', 'esp32', 'raspberry'),
    '开源': ('open source', 'open-source', 'github', '开源'),
    '机器人': ('robot', 'robotics', '机器人'),
    '半导体': ('semiconductor', 'chip', '半导体'),
}


class PlainText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.hidden = 0

    def handle_starttag(self, tag, attrs):
        if tag in ('script', 'style'):
            self.hidden += 1

    def handle_endtag(self, tag):
        if tag in ('script', 'style') and self.hidden:
            self.hidden -= 1

    def handle_data(self, text):
        if not self.hidden:
            self.parts.append(text)


def plain(text, limit):
    parser = PlainText()
    parser.feed((text or '')[:65536])
    return re.sub(r'\s+', ' ', ' '.join(parser.parts)).strip()[:limit]


def canonical(url):
    parts = urlsplit(url)
    if parts.scheme != 'https' or not parts.hostname or parts.username or parts.password:
        raise ValueError('Only public HTTPS article links are accepted')
    query = [(k, v) for k, v in parse_qsl(parts.query) if not k.lower().startswith('utm_')]
    result = urlunsplit((parts.scheme, parts.netloc, parts.path, urlencode(query), ''))
    if len(result.encode()) > 768:
        raise ValueError('Article URL too long')
    return result


def local_name(tag):
    """Atom feeds carry an XML namespace; RSS items usually do not."""
    return tag.rsplit('}', 1)[-1]


def inner_text(node):
    """Flatten Atom XHTML/HTML while keeping nested text and dropping scripts."""
    parts, stack = [], [node]
    while stack:
        current = stack.pop()
        if isinstance(current, str):
            parts.append(current)
            continue
        if local_name(current.tag) in ('script', 'style'):
            if current.tail:
                stack.append(current.tail)
            continue
        if current.text:
            parts.append(current.text)
        if current is not node and current.tail:
            stack.append(current.tail)
        for child in reversed(list(current)):
            stack.append(child)
    return ' '.join(parts)


def child_text(item, *names):
    """Pick by field priority, never by the order fields appear in the document."""
    for name in names:
        for child in item:
            if local_name(child.tag) != name:
                continue
            value = inner_text(child).strip()
            if value:
                return value
    return ''


def entry_link(item):
    """Only the article's alternate link counts; self/enclosure links are not articles."""
    for child in item:
        if local_name(child.tag) != 'link':
            continue
        if (child.get('rel') or 'alternate').strip().lower() != 'alternate':
            continue
        href = (child.get('href') or '').strip()
        if href:
            return href
    return ''


def published_epoch(text):
    """RFC 822 (RSS) or ISO 8601 (Atom); anything else is skipped."""
    text = (text or '').strip()
    if not text:
        raise ValueError('missing publication date')
    try:
        when = parsedate_to_datetime(text)
    except (TypeError, ValueError):
        when = datetime.fromisoformat(text.replace('Z', '+00:00'))
    if when.tzinfo is None:
        when = when.replace(tzinfo=timezone.utc)
    return int(when.timestamp())


def read_feed(name, url, now, opener=urlopen):
    """Stop after 15 completed items; never load the entire historical feed."""
    parser = ET.XMLPullParser(events=('end',))
    rows, used, count, tail = [], 0, 0, b''
    deadline = time.monotonic() + 12
    with opener(Request(url, headers={'User-Agent': 'ZhaoxiMorning/1.0'}), timeout=4) as response:
        if urlsplit(response.geturl()).scheme != 'https':
            raise ValueError('Insecure feed redirect')
        while count < 15 and time.monotonic() < deadline:
            chunk = response.read(8192)
            if not chunk:
                break
            used += len(chunk)
            if used > MAX_XML:
                raise ValueError('Feed exceeds byte budget')
            scan = (tail + chunk).upper()
            if b'<!DOCTYPE' in scan or b'<!ENTITY' in scan:
                raise ValueError('DTD and entity definitions are not accepted')
            tail = scan[-16:]
            parser.feed(chunk)
            for _, item in parser.read_events():
                kind = local_name(item.tag)
                if kind not in ('item', 'entry'):
                    continue
                count += 1
                try:
                    if kind == 'item':
                        epoch = published_epoch(child_text(item, 'pubDate'))
                        link = canonical(child_text(item, 'link'))
                        description = plain(child_text(item, 'description'), 240)
                    else:
                        epoch = published_epoch(child_text(item, 'published', 'updated'))
                        link = canonical(entry_link(item))
                        description = plain(child_text(item, 'summary', 'content'), 240)
                    title = plain(child_text(item, 'title'), 120)
                    if title and description and now - 172800 <= epoch <= now + 300:
                        rows.append({'id': hashlib.sha256(link.encode()).hexdigest(),
                                     'title': title, 'url': link, 'source': name[:40],
                                     'published_at': epoch, 'description': description})
                except (ValueError, TypeError, OverflowError):
                    pass
                item.clear()
                if count >= 15:
                    break
    return rows


def collect(topics, now, reader=read_feed):
    terms = set()
    for topic in topics:
        terms.update(ALIASES.get(topic.lower(), (topic.lower(),)))
    rows, errors = [], []

    def fetch(feed):
        name, url = feed
        try:
            return reader(name, url, now), None
        except Exception as error:
            return [], name + ': ' + type(error).__name__

    with ThreadPoolExecutor(max_workers=min(8, len(FEEDS))) as pool:
        for result, error in pool.map(fetch, FEEDS):
            rows.extend(result)
            if error:
                errors.append(error)
    selected, ids, titles = [], set(), set()
    by_source = {}
    for row in sorted(rows, key=lambda x: x['published_at'], reverse=True):
        text = (row['title'] + ' ' + row['description']).lower()
        relevant = not terms or any(
            (re.search(r'(?<![a-z])' + re.escape(term) + r'(?![a-z])', text)
             if term.isascii() else term in text) for term in terms)
        title_key = re.sub(r'\W+', '', row['title'].lower())
        source = row['source']
        if not relevant or row['id'] in ids or title_key in titles:
            continue
        if by_source.get(source, 0) >= 3:
            continue
        selected.append(row)
        ids.add(row['id'])
        titles.add(title_key)
        by_source[source] = by_source.get(source, 0) + 1
        if len(selected) == 10:
            break
    if not selected and errors:
        raise RuntimeError('No usable candidates; ' + '; '.join(errors))
    return {'schema': 1, 'generated_at': now, 'items': selected}


class FeedServer(ThreadingHTTPServer):
    daemon_threads = True


class FeedCompanion:
    """Own a lazy LAN listener for exactly one PPP gateway session."""
    def __init__(self, address=('10.254.42.1', 8766), factory=None,
                 clock=time.monotonic, emit=print):
        self.address = address
        self.factory = factory or FeedServer
        self.clock = clock
        self.emit = emit
        self.server = self.thread = None
        self.next_try = 0
        self.last_error = None

    def tick(self):
        if self.server or self.clock() < self.next_try:
            return
        try:
            server = self.factory(self.address, Handler)
        except OSError as error:
            # ppp42 acquires its local IP after link negotiation, not launch.
            waiting = error.errno == errno.EADDRNOTAVAIL
            self.next_try = self.clock() + (1 if waiting else 30)
            if not waiting and str(error) != self.last_error:
                self.emit('MORNING_FEED_UNAVAILABLE ' + type(error).__name__, flush=True)
            self.last_error = str(error)
            return
        self.server = server
        self.thread = threading.Thread(target=server.serve_forever,
                                       kwargs={'poll_interval': .25}, daemon=True)
        self.thread.start()
        self.last_error = None
        self.emit('MORNING_FEED_READY ' + self.address[0] + ':' + str(self.address[1]), flush=True)

    def close(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join(2)
            self.server = self.thread = None


class Handler(BaseHTTPRequestHandler):
    slots = threading.BoundedSemaphore(2)
    cache = {}
    cache_lock = threading.Lock()

    def setup(self):
        super().setup()
        self.connection.settimeout(8)

    def answer(self, code, data):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.client_address[0] not in ('127.0.0.1', '10.254.42.2'):
            return self.answer(403, {'error': 'Attached board only'})
        if self.path != '/candidates':
            return self.answer(404, {'error': 'Unknown path'})
        if not self.slots.acquire(blocking=False):
            return self.answer(503, {'error': 'Feed collector busy'})
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 1 <= length <= 4096 or self.headers.get('Transfer-Encoding'):
                raise ValueError('Invalid request size')
            value = json.loads(self.rfile.read(length))
            topics = value.get('topics', [])
            if not isinstance(topics, list) or len(topics) > 10 or any(
                    not isinstance(x, str) or not 1 <= len(x) <= 32 for x in topics):
                raise ValueError('Invalid topics')
            now = int(time.time())
            supplied = value.get('now')
            if not isinstance(supplied, (float, int)) or isinstance(supplied, bool) or abs(supplied-now) > 600:
                raise ValueError('Board clock not synchronized')
            key = tuple(sorted(topics))
            with self.cache_lock:
                saved = self.cache.get(key)
            if saved and now-saved[0] < 900:
                result = saved[1]
            else:
                result = collect(topics, now)
                with self.cache_lock:
                    self.cache[key] = (now, result)
                    if len(self.cache) > 32:
                        self.cache.pop(next(iter(self.cache)))
            if len(json.dumps(result, ensure_ascii=False).encode()) >= 24000:
                raise RuntimeError('Candidate packet exceeds board budget')
            self.answer(200, result)
        except (ValueError, TypeError, json.JSONDecodeError):
            self.answer(400, {'error': 'Invalid request'})
        except Exception as error:
            self.answer(503, {'error': type(error).__name__})
        finally:
            self.slots.release()

    def log_message(self, format, *args):
        pass  # Never log user preferences or request bodies.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='10.254.42.1')
    parser.add_argument('--port', type=int, default=8766)
    args = parser.parse_args()
    server = FeedServer((args.bind, args.port), Handler)
    print('MORNING_FEED_READY ' + args.bind + ':' + str(args.port), flush=True)
    try:
        server.serve_forever(poll_interval=.5)
    finally:
        server.server_close()


if __name__ == '__main__':
    main()

"""Official RSS parsing and candidate boundaries; no live network."""
from email.utils import formatdate
import io
import errno
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from morning_feed import canonical, collect, read_feed, FeedCompanion

class Response(io.BytesIO):
    def geturl(self):return 'https://example.org/feed'

def rss(now, **changes):
    values = dict(title='AI embedded tools', description='An open-source hardware project',
                  link='https://example.org/article?utm_source=x', pubDate=formatdate(now, usegmt=True))
    values.update(changes)
    body = ''.join('<'+k+'><![CDATA['+v+']]></'+k+'>' for k, v in values.items())
    return ('<rss><channel><item>'+body+'</item></channel></rss>').encode()

class FeedTests(unittest.TestCase):
    def test_canonical_tracking_and_https(self):
        self.assertEqual(canonical('https://example.org/a?utm_source=x&id=2#part'), 'https://example.org/a?id=2')
        for url in ('http://example.org/a','file:///etc/passwd','https://u:p@example.org/a'):
            with self.assertRaises(ValueError):canonical(url)

    def test_age_future_text_and_real_metadata(self):
        now=1793574000
        def parse(**changes):return read_feed('Official','https://example.org/feed',now,
            opener=lambda *a,**k:Response(rss(now,**changes)))
        row=parse(description='<script>bad</script><p>Good AI content</p>')[0]
        self.assertEqual(row['description'],'Good AI content')
        self.assertEqual(len(row['id']),64)
        self.assertEqual(row['url'],'https://example.org/article')
        self.assertEqual(parse(pubDate=formatdate(now-172801,usegmt=True)),[])
        self.assertEqual(parse(pubDate=formatdate(now+301,usegmt=True)),[])

    def test_entity_definitions_rejected(self):
        xml=b'<!DOCTYPE rss [<!ENTITY x "danger">]><rss><channel/></rss>'
        with self.assertRaises(ValueError):
            read_feed('Official','https://example.org/feed',1793574000,lambda *a,**k:Response(xml))

    def test_duplicate_sources_keyword_boundaries_and_failure(self):
        now=1793574000
        rows=read_feed('Official','https://example.org/feed',now,lambda *a,**k:Response(rss(now)))
        self.assertEqual(len(collect(['AI'],now,lambda *a:rows)['items']),1)
        other=[dict(rows[0],title='It said hello',description='A plain greeting')]
        self.assertEqual(collect(['AI'],now,lambda *a:other)['items'],[])
        def fail(*args):raise OSError('offline')
        with self.assertRaises(RuntimeError):collect(['AI'],now,fail)

    def test_companion_waits_for_ppp_then_owns_one_listener(self):
        now=[0];calls=[];events=[]
        class Server:
            def serve_forever(self, **kwargs):events.append('serve')
            def shutdown(self):events.append('shutdown')
            def server_close(self):events.append('close')
        def factory(*args):
            calls.append(args)
            if len(calls)==1:raise OSError(errno.EADDRNOTAVAIL,'PPP not ready')
            return Server()
        companion=FeedCompanion(factory=factory,clock=lambda:now[0],emit=lambda *a,**k:None)
        companion.tick();companion.tick();self.assertEqual(len(calls),1)
        now[0]=1;companion.tick();companion.thread.join(2)
        companion.tick();self.assertEqual(len(calls),2)
        companion.close();companion.close()
        self.assertEqual(events,['serve','shutdown','close'])

if __name__=='__main__':unittest.main()

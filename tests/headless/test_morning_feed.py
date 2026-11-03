"""Official RSS parsing and candidate boundaries; no live network."""
from datetime import datetime, timezone
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

    def test_atom_entries_are_read(self):
        now=1793574000
        stamp=datetime.fromtimestamp(now,timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        atom=('<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><entry>'
              '<title>New open source model</title>'
              '<link rel="self" href="https://example.org/feed"/>'
              '<link rel="alternate" href="https://example.org/article"/>'
              '<published>'+stamp+'</published>'
              '<summary><![CDATA[<p>An AI language model release</p>]]></summary>'
              '</entry></feed>').encode()
        rows=read_feed('Atom','https://example.org/feed',now,lambda *a,**k:Response(atom))
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['url'],'https://example.org/article')
        self.assertEqual(rows[0]['title'],'New open source model')
        self.assertEqual(rows[0]['description'],'An AI language model release')

    def test_atom_xhtml_summary_keeps_nested_text(self):
        now=1793574000
        stamp=datetime.fromtimestamp(now,timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        atom=('<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><entry>'
              '<title>New open source model</title>'
              '<link rel="self" href="https://example.org/feed"/>'
              '<link rel="alternate" href="https://example.org/xhtml"/>'
              '<published>'+stamp+'</published>'
              '<summary type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml">'
              '<p>An <b>AI language</b> model release<script>bad</script></p>'
              '</div></summary></entry></feed>').encode()
        rows=read_feed('Atom','https://example.org/feed',now,lambda *a,**k:Response(atom))
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['description'],'An AI language model release')

    def test_atom_priority_and_content_fallback(self):
        now=1793574000
        def at(offset):return datetime.fromtimestamp(now+offset,timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        atom=('<feed xmlns="http://www.w3.org/2005/Atom">'
              '<entry><title>Priority fields</title>'
              '<link rel="self" href="https://example.org/feed"/>'
              '<link rel="alternate" href="https://example.org/priority"/>'
              '<updated>'+at(-7200)+'</updated>'
              '<published>'+at(-60)+'</published>'
              '<content type="html"><![CDATA[<p>Fallback content</p>]]></content>'
              '<summary><![CDATA[<p>Preferred summary</p>]]></summary></entry>'
              '<entry><title>Fallback fields</title>'
              '<link rel="alternate" href="https://example.org/fallback"/>'
              '<updated>'+at(-120)+'</updated>'
              '<summary/>'
              '<content type="html"><![CDATA[<p>Content fallback</p>]]></content>'
              '</entry></feed>').encode()
        rows=read_feed('Atom','https://example.org/feed',now,lambda *a,**k:Response(atom))
        self.assertEqual([r['description'] for r in rows],['Preferred summary','Content fallback'])
        self.assertEqual([r['published_at'] for r in rows],[now-60,now-120])

    def test_atom_self_link_is_not_an_article(self):
        now=1793574000
        stamp=datetime.fromtimestamp(now,timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        atom=('<feed xmlns="http://www.w3.org/2005/Atom"><entry>'
              '<title>Only a feed link</title>'
              '<link rel="self" href="https://example.org/feed"/>'
              '<published>'+stamp+'</published>'
              '<summary>Feed metadata</summary></entry></feed>').encode()
        self.assertEqual(read_feed('Atom','https://example.org/feed',now,
            lambda *a,**k:Response(atom)),[])

    def test_source_cap_keeps_global_limit_and_order(self):
        now=1793574000
        def row(source,index):
            return {'id':source+str(index),
                    'title':'AI hardware '+source+' '+str(index),
                    'url':'https://example.org/'+source.lower().replace(' ','-')+'/'+str(index),
                    'source':source,'published_at':now-index,
                    'description':'embedded AI'}
        def one_source(name,url,now):
            return [row('Hackaday',i) for i in range(12)] if name=='Hackaday' else []
        capped=collect([],now,one_source)['items']
        self.assertEqual(len(capped),3)
        self.assertEqual([x['source'] for x in capped],['Hackaday']*3)

        sources=('Hackaday','CNX Software','Adafruit','Seeed Studio')
        def many_sources(name,url,now):
            if name not in sources:return []
            offset=sources.index(name)
            return [dict(row(name,i),published_at=now-(i*10+offset)) for i in range(4)]
        items=collect([],now,many_sources)['items']
        self.assertEqual(len(items),10)
        counts={name:sum(x['source']==name for x in items) for name in sources}
        self.assertEqual(counts['Hackaday'],3)
        self.assertLessEqual(max(counts.values()),3)
        self.assertEqual([x['published_at'] for x in items],
                         sorted((x['published_at'] for x in items),reverse=True))

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

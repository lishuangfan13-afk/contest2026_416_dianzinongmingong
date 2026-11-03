import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from urllib.error import HTTPError
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import export_morning_feed as export
import publish_morning_feed as publish

class StaticFeedTests(unittest.TestCase):
    def test_export_has_public_topics_and_byte_budget(self):
        def collect(topics,now):
            self.assertIn('AI',topics);self.assertIn('电子',topics)
            return {'schema':1,'generated_at':now,'items':[]}
        self.assertEqual(json.loads(export.snapshot(123,collect))['generated_at'],123)
        with self.assertRaises(ValueError):
            export.snapshot(123,lambda *a:{'items':['x'*24000]})

    def test_source_failure_keeps_old_snapshot(self):
        with tempfile.TemporaryDirectory() as folder:
            target=Path(folder)/'candidates.json';target.write_bytes(b'old')
            with patch.object(sys,'argv',['export','--output',str(target)]),patch.object(export,'snapshot',side_effect=RuntimeError):
                with self.assertRaises(RuntimeError):export.main()
            self.assertEqual(target.read_bytes(),b'old')

    def publish_fixture(self,value,missing=False,pages=False,missing_pages=False,wrong_pages=False):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        path=Path(temp.name)/'candidates.json';path.write_text(json.dumps(value))
        calls=[]
        def urlopen(request,timeout):
            self.assertEqual(timeout,20);self.assertEqual(request.headers['Authorization'],'Bearer test-only')
            body=json.loads(request.data) if request.data else None
            calls.append((request.get_method(),request.full_url,body))
            if missing and request.get_method()=='GET' and '/git/ref/heads/' in request.full_url:
                raise HTTPError(request.full_url,404,'missing',{},None)
            if request.get_method()=='PUT':self.assertEqual(body['branch'],'news-site')
            if request.full_url.endswith('/pages'):
                if missing_pages:raise HTTPError(request.full_url,404,'missing',{},None)
                return contextlib.closing(io.BytesIO(json.dumps({'source':{'branch':'other' if wrong_pages else 'news-site','path':'/'}}).encode()))
            return contextlib.closing(io.BytesIO(json.dumps({'sha':'old'}).encode()))
        with patch.object(sys,'argv',['publish','--input',str(path)]),patch.dict(os.environ,{
            'GITHUB_REPOSITORY':'owner/example','GITHUB_TOKEN':'test-only','GITHUB_PAGES_PUBLISH':'1' if pages else '0'}),patch.object(publish,'urlopen',urlopen):
            with contextlib.redirect_stdout(io.StringIO()):publish.main()
        return calls

    def test_only_candidate_file_is_written_to_feed_branch(self):
        calls=self.publish_fixture({'schema':1,'generated_at':123,'items':[]})
        writes=[call for call in calls if call[0]=='PUT']
        self.assertEqual(len(writes),1)
        self.assertTrue(writes[0][1].endswith('/contents/candidates.json'))
        self.assertEqual(writes[0][2]['sha'],'old')

    def test_first_publish_is_a_data_only_orphan_and_requests_pages_build(self):
        calls=self.publish_fixture({'schema':1,'generated_at':123,'items':[]},missing=True,pages=True)
        tree=next(body for method,url,body in calls if url.endswith('/git/trees'))
        self.assertNotIn('base_tree',tree)
        self.assertEqual({entry['path'] for entry in tree['tree']},{'candidates.json','.nojekyll'})
        commit=next(body for method,url,body in calls if url.endswith('/git/commits'))
        self.assertEqual(commit['parents'],[])
        self.assertTrue(any(method=='POST' and url.endswith('/pages/builds') for method,url,body in calls))

    def test_pages_not_configured_saves_snapshot_without_build(self):
        calls=self.publish_fixture({'schema':1,'generated_at':123,'items':[]},pages=True,missing_pages=True)
        self.assertTrue(any(method=='PUT' for method,url,body in calls))
        self.assertFalse(any(url.endswith('/pages/builds') for method,url,body in calls))

    def test_unrelated_pages_source_is_not_rebuilt(self):
        with self.assertRaises(RuntimeError):
            self.publish_fixture({'schema':1,'generated_at':123,'items':[]},pages=True,wrong_pages=True)

    def test_private_extra_fields_rejected_before_network(self):
        for value in [
            {'schema':1,'generated_at':123,'items':[],'persona':'private'},
            {'schema':1,'generated_at':123,'items':[{'api_key':'never publish'}]},
            {'schema':1,'generated_at':123,'items':[{}]*11}]:
            with self.assertRaises(ValueError):self.publish_fixture(value)

if __name__=='__main__':unittest.main()

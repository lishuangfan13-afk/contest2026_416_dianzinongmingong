#!/usr/bin/env python3
"""Publish only a validated candidate JSON to the dedicated news-feed branch."""
import argparse
import base64
import json
import os
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request,urlopen

BRANCH='news-site'
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',type=Path,required=True)
    a=p.parse_args();data=a.input.read_bytes()
    if not 1<=len(data)<24000:raise ValueError('Snapshot byte budget')
    value=json.loads(data)
    if set(value)!={'schema','generated_at','items'} or value['schema']!=1:
        raise ValueError('Only the public feed schema may be published')
    if not isinstance(value['items'],list) or len(value['items'])>10:raise ValueError('Candidate limit')
    expected={'id','title','url','source','published_at','description'}
    if any(not isinstance(row,dict) or set(row)!=expected for row in value['items']):
        raise ValueError('Only public candidate fields may be published')
    repository=os.environ['GITHUB_REPOSITORY'];token=os.environ['GITHUB_TOKEN']
    def api(path,body=None,method=None,missing=False):
        request=Request('https://api.github.com/repos/'+repository+path,
            data=None if body is None else json.dumps(body).encode(),method=method,
            headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json',
                     'Content-Type':'application/json','User-Agent':'Zhaoxi-Public-Feed'})
        try:
            with urlopen(request,timeout=20) as response:return json.load(response)
        except HTTPError as error:
            if missing and error.code==404:return None
            raise RuntimeError('GitHub API HTTP '+str(error.code)) from None
    if api('/git/ref/heads/'+BRANCH,missing=True) is None:
        # An orphan data-only branch prevents the project tree being served
        # by Pages. Never derive the publishing tree from the source branch.
        entries=[]
        for path,content in [('candidates.json',data),('.nojekyll',b'')]:
            blob=api('/git/blobs',{'content':base64.b64encode(content).decode(),'encoding':'base64'},'POST')
            entries.append({'path':path,'mode':'100644','type':'blob','sha':blob['sha']})
        tree=api('/git/trees',{'tree':entries},'POST')
        commit=api('/git/commits',{'message':'Initialize public news-only Pages branch',
            'tree':tree['sha'],'parents':[]},'POST')
        api('/git/refs',{'ref':'refs/heads/'+BRANCH,'sha':commit['sha']},'POST')
    else:
        previous=api('/contents/candidates.json?ref='+BRANCH,missing=True)
        body={'message':'Refresh public morning news candidates','branch':BRANCH,
              'content':base64.b64encode(data).decode()}
        if previous:body['sha']=previous['sha']
        api('/contents/candidates.json',body,'PUT')
    if os.environ.get('GITHUB_PAGES_PUBLISH')=='1':
        site=api('/pages',missing=True)
        if site is None:
            print('Snapshot saved; configure Pages source news-site / before requesting deployment.')
            return
        if site.get('source',{}).get('branch')!=BRANCH or site['source'].get('path')!='/':
            raise RuntimeError('Pages uses a different source; refusing to rebuild unrelated hosting')
        # GITHUB_TOKEN updates do not automatically trigger a branch Pages
        # build. Request it explicitly with the workflow's pages permission.
        api('/pages/builds',{},'POST')
    print('Published public feed snapshot to news-site/candidates.json')

if __name__=='__main__':main()

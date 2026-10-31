"""Compile unchanged functions extracted from the actual NuttX tree.

No sockets/network/board are used: only POSIX boundaries are replaced.
These regression cases verify partial TCP I/O, socket cleanup and ordinary DNS parsing. Passing them does not prove board Internet access.
"""
from pathlib import Path
import json
import subprocess
import difflib
import hashlib
import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--nuttx-root', type=Path, required=True)
args = parser.parse_args()
out = Path(__file__).resolve().parents[1] / 'out/headless/dns'
out.mkdir(parents=True, exist_ok=True)
source_bytes = (args.nuttx_root / 'libs/libc/netdb/lib_dnsquery.c').read_bytes()
source_raw = source_bytes.decode()
src = source_raw.replace('\r\n', '\n')
dns_h = (args.nuttx_root / 'include/nuttx/net/dns.h').read_text()

def function(name):
    pos = src.index('\nstatic ', src.index('Private Functions'))
    while True:
        begin = pos + 1
        brace = src.index('{', begin)
        if name+'(' in src[begin:brace]:
            depth = 1
            end = brace + 1
            while depth:
                depth += (src[end] == '{') - (src[end] == '}')
                end += 1
            return src[begin:end]+'\n'
        pos = src.index('\nstatic ', brace)

common = r'''
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#define FAR
#define OK 0
#define CONFIG_NET_IPv4 1
#define CONFIG_NET_IPv6 1
#define CONFIG_NETDB_DNSCLIENT_NAMESIZE 512
#define CONFIG_NETDB_DNSCLIENT_MAXRESPONSE 512
#define CONFIG_NETDB_MAX_IPv4ADDR 1
#define CONFIG_NETDB_MAX_IPv6ADDR 1
#define CONFIG_NETDB_DNSCLIENT_ENTRIES 0
#define CONFIG_NETDB_DNSCLIENT_RETRIES 3
#define RECV_BUFFER_SIZE 512
#define QUERY_BUFFER_SIZE 530
#define HTONS htons
#define NTOHS ntohs
#define DEBUGASSERT assert
#define get_errno() errno
#define set_errno(x) (errno=(x))
#define ninfo(...) ((void)0)
#define nwarn(...) ((void)0)
#define nerr(...) ((void)0)
#define DNS_CLASS_IN 1
#define DNS_RECTYPE_A 1
#define DNS_RECTYPE_AAAA 28
#define DNS_FLAG1_RESPONSE 0x80
#define DNS_FLAG1_TRUNC 0x02
#define DNS_FLAG2_ERR_MASK 0x0f
union dns_addr_u {
 struct sockaddr addr;
 struct sockaddr_in ipv4;
 struct sockaddr_in6 ipv6;
};
'''
for struct in ('dns_header_s', 'dns_question_s', 'dns_answer_s'):
    begin = dns_h.index('begin_packed_struct struct '+struct)
    end = dns_h.index('end_packed_struct;', begin)+len('end_packed_struct;')
    common += dns_h[begin:end].replace('begin_packed_struct ', '').replace('end_packed_struct', '__attribute__((packed))')+'\n'
begin = src.index('struct dns_query_s\n')
end = src.index('/****************************************************************************', begin)
common += src[begin:end]

cases = {}
cases['short_stream_io'] = common+r'''
static unsigned char source[]="abcdefgh";
static unsigned char *recv_base;
static const unsigned char *send_base;
static int send_calls,recv_calls;
static long second_send_offset;
static ssize_t fake_send(int fd,const void *p,size_t n,int flags) {
 (void)fd;(void)flags;
 if(send_calls++==0){send_base=p;return 3;}
 second_send_offset=(const unsigned char*)p-send_base;
 return n;
}
static ssize_t fake_recv(int fd,void *p,size_t n,int flags) {
 (void)fd;(void)flags;
 if(recv_calls++==0){recv_base=p;memcpy(p,source,3);return 3;}
 memcpy(p,source+3,n);return n;
}
#define send fake_send
#define recv fake_recv
'''+function('stream_send')+function('stream_recv')+r'''
int main(void) {
 unsigned char a[16]="abcdefgh",b[16]={0};
 ssize_t sent=stream_send(1,a,8),received=stream_recv(1,b,8);
 printf("send_result=%zd second_write_offset=%ld expected=3\n",sent,second_send_offset);
 printf("recv_result=%zd matches_expected=%d gap_byte3=%d tail_at8=%c\n",received,!memcmp(b,source,8),b[3],b[8]);
 assert(sent==8 && second_send_offset==3);
 assert(received==8 && memcmp(b,source,8)==0 && b[8]==0);
 return 0;
}
'''

cases['truncation_socket_leak'] = common+r'''
static int binds,closes,receives;
static bool dns_is_queryfamily(int family){return family==AF_INET;}
static int dns_bind(int family,bool stream){(void)family;(void)stream;return ++binds+10;}
static int fake_close(int fd){(void)fd;++closes;return 0;}
static int dns_send_query(int fd,const char *name,union dns_addr_u *server,uint16_t type,
 struct dns_query_info_s *info,uint8_t *buf,bool stream) {
 (void)fd;(void)name;(void)server;(void)type;(void)info;(void)buf;(void)stream;return 0;
}
static int dns_recv_response(int fd,union dns_addr_u *addr,int naddr,
 struct dns_query_info_s *info,uint32_t *ttl,uint8_t *buf,bool stream,bool *retry) {
 (void)fd;(void)addr;(void)naddr;(void)info;(void)ttl;(void)buf;
 if(receives++==0){assert(!stream);*retry=true;return -EAGAIN;}
 assert(stream);return 1;
}
static void dns_query_error(const char *prompt,int rc,union dns_addr_u *server){(void)prompt;(void)rc;(void)server;}
#define close fake_close
'''+function('dns_query_callback')+r'''
int main(void) {
 union dns_addr_u addr[2]={0},server={0}; int count=2;
 struct dns_query_data_s q={0};
 q.query.hostname="a.test";q.query.addr=addr;q.query.naddr=&count;
 server.ipv4.sin_family=AF_INET;
 int rc=dns_query_callback(&q,&server.addr,sizeof(server.ipv4));
 printf("callback_result=%d sockets_created=%d sockets_closed=%d leaked=%d\n",rc,binds,closes,binds-closes);
 assert(rc==1 && binds==2 && closes==2);
 return 0;
}
'''

cases['ordinary_dns_parse_controls'] = common+r'''
static uint8_t packet[512];static size_t packet_len;
static ssize_t fake_recv(int fd,void *buf,size_t n,int flags){(void)fd;(void)flags;assert(n>=packet_len);memcpy(buf,packet,packet_len);return packet_len;}
static ssize_t stream_recv_record(int fd,void *buf,size_t n){return fake_recv(fd,buf,n,0);}
#define recv fake_recv
'''+function('dns_parse_name')+function('dns_recv_response')+r'''
static void put16(size_t pos,uint16_t v){packet[pos]=v>>8;packet[pos+1]=v;}
static void question(struct dns_query_info_s *q,int answers){
 memset(packet,0,sizeof(packet));
 put16(0,0x1234);packet[2]=0x81;packet[3]=0x80;put16(4,1);put16(6,answers);
 static const uint8_t name[]={1,'a',4,'t','e','s','t',0};
 memcpy(packet+12,name,sizeof(name));put16(20,1);put16(22,1);
 memset(q,0,sizeof(*q));q->id=htons(0x1234);q->rectype=htons(1);q->qnamelen=7;memcpy(q->qname,name,sizeof(name));
 packet_len=24;
}
static void answer(uint8_t name_offset,uint16_t type,const uint8_t *rdata,size_t len){
 size_t p=packet_len;packet[p]=0xc0;packet[p+1]=name_offset;put16(p+2,type);put16(p+4,1);
 packet[p+9]=60;put16(p+10,len);memcpy(packet+p+12,rdata,len);packet_len+=12+len;
}
static int parse(struct dns_query_info_s *q,bool *tcp){union dns_addr_u addr[2]={0};uint8_t buf[530];uint32_t ttl;
 int rc=dns_recv_response(1,addr,1,q,&ttl,buf,false,tcp);
 if(rc>0){assert(addr[0].ipv4.sin_addr.s_addr==htonl(0xcb007107));}
 return rc;
}
int main(void){
 struct dns_query_info_s q;uint8_t ip[]={203,0,113,7};bool tcp=false;
 question(&q,1);answer(12,1,ip,4);int a=parse(&q,&tcp);assert(a==1 && !tcp);
 question(&q,2);uint8_t alias[]={1,'b',0xc0,14};answer(12,5,alias,sizeof(alias));answer(36,1,ip,4);
 int cname=parse(&q,&tcp);assert(cname==1);
 question(&q,0);packet[2]|=2;int tc=parse(&q,&tcp);assert(tc==-EAGAIN && tcp);
 question(&q,0);packet[3]=0x83;tcp=false;int nx=parse(&q,&tcp);assert(nx==-EPROTO && !tcp);
 printf("A=%d compressed_CNAME_then_A=%d TC=%d tcp_requested=%d NXDOMAIN=%d\n",a,cname,tc,1,nx);
 return 0;
}
'''

# Replay the recorded public MiMo reply as well as the synthetic controls.
# This still replaces recv(), so it tests parsing and not radio delivery.
fixture = Path(__file__).resolve().parents[1] / 'tests/headless/wifi/mimo_dns_reply_20261031.txt'
raw = bytes.fromhex(fixture.read_text())
end = 12
while raw[end]:
    label = raw[end]
    if label > 63 or end + label + 1 >= len(raw):
        raise ValueError('Invalid fixture question')
    end += label + 1
question = raw[12:end + 1]
literal = ','.join(str(byte) for byte in raw)
cases['actual_mimo_reply_replay'] = common + r'''
static const uint8_t packet[] = {''' + literal + r'''};
static ssize_t fake_recv(int fd,void *buf,size_t n,int flags) {
 (void)fd;(void)flags;assert(n>=sizeof(packet));memcpy(buf,packet,sizeof(packet));return sizeof(packet);
}
static ssize_t stream_recv_record(int fd,void *buf,size_t n){return fake_recv(fd,buf,n,0);}
#define recv fake_recv
''' + function('dns_parse_name') + function('dns_recv_response') + r'''
int main(void) {
 struct dns_query_info_s q={0};union dns_addr_u addr[2]={0};
 memcpy(&q.id,packet,2);
 q.qnamelen=''' + str(len(question)-1) + r''';
 memcpy(q.qname,packet+12,''' + str(len(question)) + r''');
 memcpy(&q.rectype,packet+''' + str(end+1) + r''',2);
 uint8_t buf[530];uint32_t ttl=0;bool tcp=false;
 int rc=dns_recv_response(1,addr,1,&q,&ttl,buf,false,&tcp);
 assert(rc==1 && !tcp && addr[0].ipv4.sin_addr.s_addr!=0);
 printf("recorded_MiMo_reply_bytes=%zu result=%d address=%s TCP_requested=%d\n",
        sizeof(packet),rc,inet_ntoa(addr[0].ipv4.sin_addr),tcp);
 return 0;
}
'''

report = {'source_sha256':hashlib.sha256(source_bytes).hexdigest(),'cases':{}}
for name, code in cases.items():
    path = out/('dns_'+name+'.c')
    path.write_text(code)
    binary = path.with_suffix('')
    build = subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(path),'-o',str(binary)],capture_output=True,text=True)
    if build.returncode:
        print(build.stderr)
        raise SystemExit(build.returncode)
    run = subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    report['cases'][name]={'exit_code':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
    print(name,run.returncode,run.stdout,run.stderr,flush=True)
(out/'dns_repro_results.json').write_text(json.dumps(report,indent=2))
raise SystemExit(any(r['exit_code'] for r in report['cases'].values()))

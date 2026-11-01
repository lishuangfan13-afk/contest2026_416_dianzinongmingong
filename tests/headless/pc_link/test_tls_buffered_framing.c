/* SPDX-License-Identifier: Apache-2.0 */
/* Include the complete unchanged production translation unit. Only TLS IO is
 * substituted; all buffered/shared/stream framing functions are actual code. */
#define mbedtls_ssl_read fixture_ssl_read
#include VELA_TLS_SOURCE
#undef mbedtls_ssl_read
#include <assert.h>
#ifdef OLD_TLS_RAW_SIGNATURE
#define tls_read_response(c,b,s,n,k,h) tls_read_response(c,b,s,n,k)
#endif

static struct {
    const unsigned char *bytes;
    size_t length, position, fragment;
    unsigned after_end;
    int end_result;
} feed;

int fixture_ssl_read(mbedtls_ssl_context *ctx, unsigned char *bytes, size_t capacity)
{
    (void)ctx;
    if (feed.position == feed.length) { feed.after_end++; return feed.end_result; }
    size_t n = feed.length - feed.position;
    if (n > feed.fragment) n = feed.fragment;
    if (n > capacity) n = capacity;
    memcpy(bytes, feed.bytes + feed.position, n); feed.position += n;
    return (int)n;
}

static int run(const unsigned char *wire, size_t length, size_t fragment,
               char *output, size_t capacity, size_t *used, bool *keep, int ending)
{
    feed = (typeof(feed)){wire,length,0,fragment,0,ending};
    tls_ctx_t ctx = {0};
    return tls_read_response(&ctx,output,capacity,used,keep,false);
}

static void expect(const char *wire, int expected, const char *body, unsigned extra)
{
    for (size_t fragment=1; fragment<=64; fragment++) {
        char output[256]; memset(output,0xa5,sizeof(output)); size_t used=123; bool keep=true;
        int status=run((const unsigned char *)wire,strlen(wire),fragment,
                       output,sizeof(output),&used,&keep,0);
        assert(status==expected);
        if (status<0) assert(!output[0] && !used && !keep);
        else { assert(body && used==strlen(body)); assert(!memcmp(output,body,used+1)); }
        if (expected>0) assert(feed.after_end==extra);
    }
}

static int reject_stream(const unsigned char *bytes,size_t count,void *user)
{ (void)bytes;(void)count;(*(unsigned *)user)++;return -ECANCELED; }

int main(void)
{
    expect("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",200,"hello",0);
    expect("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",200,"hello",0);
    expect("HTTP/1.1 404 Missing\r\nContent-Length: 5\r\n\r\nerror",404,"error",0);
    expect("HTTP/1.1 401 Denied\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=x\r\nerror\r\n0;end=x\r\nX-End: yes\r\n\r\n",401,"error",0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;ext=x\r\nhe\r\n3\r\nllo\r\n0;end=x\r\nX-End: yes\r\n\r\n",200,"hello",0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0;end=x\r\n\r\n",200,"",0);
    expect("HTTP/1.0 200 OK\r\n\r\nclose delimited",200,"close delimited",1);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",200,"",0);
    expect("HTTP/1.1 204 Empty\r\n\r\n",204,"",0);
    expect("HTTP/1.1 304 Same\r\nContent-Length: 999\r\n\r\n",304,"",0);
    expect("HTTP/1.1 304 Same\r\nContent-Length: 999999999\r\nContent-Encoding: gzip\r\n\r\n",304,"",0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n你好吗",200,"你好吗",0);
    const char *bad[]={
        "HTTP/1.1 200 OK\r\nContent-Length: garbage\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5junk\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;ext=x\r\nh",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\nBad trailer\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nBad Name: x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: \001\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhiXX0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nAAAA\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nBad Header: x\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
    };
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) expect(bad[i],VELA_TLS_ERR_PROTOCOL,NULL,0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 9999999999999999999999\r\n\r\n",VELA_TLS_ERR_OVERFLOW,NULL,0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 10485761\r\n\r\n",VELA_TLS_ERR_OVERFLOW,NULL,0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFFFF\r\n",VELA_TLS_ERR_OVERFLOW,NULL,0);
    const char *wire="HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    char output[256];size_t used=99;bool keep=true;
    assert(run((const unsigned char *)wire,strlen(wire),3,output,5,&used,&keep,0)==VELA_TLS_ERR_OVERFLOW);
    assert(!output[0]&&!used&&!keep);
    assert(run((const unsigned char *)wire,strlen(wire),3,output,6,&used,&keep,0)==200 && used==5 && !keep);
    assert(run((const unsigned char *)wire,strlen(wire),3,NULL,0,&used,&keep,0)==VELA_TLS_ERR_PROTOCOL&&!used&&!keep);
    wire="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    assert(run((const unsigned char *)wire,strlen(wire),3,output,1,&used,&keep,0)==200&&!used&&keep);
    wire="HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
    assert(run((const unsigned char *)wire,strlen(wire),3,output,sizeof(output),&used,&keep,0)==200&&keep);
    wire="HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\nContent-Length: 0\r\n\r\n";
    assert(run((const unsigned char *)wire,strlen(wire),3,output,sizeof(output),&used,&keep,0)==200&&!keep);
    wire="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi";
    assert(run((const unsigned char *)wire,strlen(wire),3,output,sizeof(output),&used,&keep,MBEDTLS_ERR_NET_RECV_FAILED)==VELA_TLS_ERR_READ&&!output[0]&&!used);
    unsigned char binary[]="HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b";
    assert(run(binary,sizeof(binary)-1,1,output,sizeof(output),&used,&keep,0)==200 && used==3 && !memcmp(output,"a\0b\0",4));
    /* Buffered callers retain their original 8KiB header budget; SSE retains
     * its 4KiB limit. Completion does not depend on the TLS record boundary. */
    char big[8200];size_t prefix=(size_t)snprintf(big,sizeof(big),"HTTP/1.1 200 OK\r\nX-Large: ");
    memset(big+prefix,'a',5000);prefix+=5000;
    strcpy(big+prefix,"\r\nContent-Length: 0\r\n\r\n");
    assert(run((const unsigned char *)big,strlen(big),19,output,sizeof(output),&used,&keep,0)==200&&!used);
    prefix=(size_t)snprintf(big,sizeof(big),"HTTP/1.1 200 OK\r\nX-Large: ");
    memset(big+prefix,'a',sizeof(big)-prefix-1);big[sizeof(big)-1]=0;
    assert(run((const unsigned char *)big,strlen(big),19,output,sizeof(output),&used,&keep,0)==VELA_TLS_ERR_OVERFLOW&&!used&&!output[0]);
    wire="HTTP/1.1 404 Missing\r\nContent-Length: 5\r\n\r\nerror";
    feed=(typeof(feed)){(const unsigned char *)wire,strlen(wire),0,1,0,0};tls_ctx_t ctx={0};unsigned calls=0;
    assert(tls_read_response_stream(&ctx,reject_stream,&calls,tls_stream_now_ms()+1000)==404&&!calls);
    wire="HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    feed=(typeof(feed)){(const unsigned char *)wire,strlen(wire),0,1,0,0};
    assert(tls_read_response_stream(&ctx,reject_stream,&calls,tls_stream_now_ms()+1000)==-ECANCELED&&calls==1);
    char informational[512]="";
    for(int i=0;i<9;i++)strcat(informational,"HTTP/1.1 103 Hint\r\n\r\n");
    expect(informational,VELA_TLS_ERR_PROTOCOL,NULL,0);
    expect("HTTP/1.1 101 Upgrade\r\n\r\n",VELA_TLS_ERR_PROTOCOL,NULL,0);
    /* HEAD's advertised representation length is metadata. If a TLS record
     * also contains an unsolicited next message, never reuse that socket. */
    wire="HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\nContent-Encoding: gzip\r\n\r\n";
    feed=(typeof(feed)){(const unsigned char *)wire,strlen(wire),0,1,0,0};
    assert(tls_read_response(&ctx,output,sizeof(output),&used,&keep,true)==200&&!used&&keep&&!feed.after_end);
    wire="HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    feed=(typeof(feed)){(const unsigned char *)wire,strlen(wire),0,512,0,0};
    assert(tls_read_response(&ctx,output,sizeof(output),&used,&keep,true)==200&&!used&&!keep&&!feed.after_end);
    puts("PASS: full production TLS unit: fragmented buffered/shared/stream framing, extensions/trailers, error bodies, early EOF, binary/UTF8, strict bounds and keep-alive.");
}

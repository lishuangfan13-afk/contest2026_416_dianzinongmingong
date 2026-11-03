/* SPDX-License-Identifier: Apache-2.0 */
/* Explicit one-shot RAM recording, for diagnosing the actual ASR input. No
 * recording unless armed; no filesystem or network writes; clear releases it. */
#include "wake_audio_diag.h"
#include "mbedtls/base64.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char *saved;
static size_t saved_bytes;
static int armed;
static uint32_t checksum(const unsigned char *p,size_t n)
{
    uint32_t crc=0xffffffffU;
    while(n--){crc^=*p++;for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320U&(0U-(crc&1)));}
    return ~crc;
}
void zhaoxi_wake_audio_capture(const void *pcm,size_t bytes)
{
    pthread_mutex_lock(&lock);
    if(armed && pcm && bytes && bytes<=384000 && !(bytes%2)) {
        saved=malloc(bytes);
        if(saved){memcpy(saved,pcm,bytes);saved_bytes=bytes;armed=0;
            printf("WAKEPCM_CAPTURED bytes=%zu crc=%08lx\n",bytes,(unsigned long)checksum(saved,bytes));}
    }
    pthread_mutex_unlock(&lock);
}
int zhaoxi_wake_audio_command(const char *op)
{
    if(!op)return -EINVAL;
    pthread_mutex_lock(&lock);int rc=0;
    if(!strcmp(op,"clear") || !strcmp(op,"arm")) {
        free(saved);saved=NULL;saved_bytes=0;armed=!strcmp(op,"arm");
        printf("WAKEPCM_STATE armed=%d bytes=0\n",armed);
    } else if(!strcmp(op,"status"))printf("WAKEPCM_STATE armed=%d bytes=%zu\n",armed,saved_bytes);
    else if(!strcmp(op,"dump")) {
        if(!saved || !saved_bytes)rc=-ENODATA;
        else {
            printf("WAKEPCM_BEGIN bytes=%zu crc=%08lx\n",saved_bytes,(unsigned long)checksum(saved,saved_bytes));
            for(size_t at=0;at<saved_bytes;at+=480) {
                size_t n=saved_bytes-at,written=0;if(n>480)n=480;
                unsigned char out[641];
                if(mbedtls_base64_encode(out,sizeof(out),&written,saved+at,n)){rc=-EIO;break;}
                out[written]=0;
                printf("WAKEPCM_DATA at=%zu n=%zu crc=%08lx data=%s\n",at,n,
                    (unsigned long)checksum(saved+at,n),out);
            }
            puts("WAKEPCM_END");
        }
    } else rc=-EINVAL;
    pthread_mutex_unlock(&lock);return rc;
}

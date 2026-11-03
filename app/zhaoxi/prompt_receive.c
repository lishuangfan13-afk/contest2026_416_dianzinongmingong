/* SPDX-License-Identifier: Apache-2.0
 * Idle-NSH-only, bounded binary asset transfer. Stop-and-wait 1024-byte
 * blocks avoid overflowing the console RX ring. No supplied remote paths.
 */
#include "local_prompts.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
static int64_t milliseconds(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
int zhaoxi_prompt_receive(const char *id,size_t length)
{
    if(!zhaoxi_prompt_text(id) || length<26 || length>480024)return -EINVAL;
    const char *dirs[]={ZHAOXI_AUDIO_DIR,ZHAOXI_PROMPT_DIR};
    for(unsigned i=0;i<2;i++)if(mkdir(dirs[i],0770) && errno!=EEXIST)return -errno;
    char path[160];snprintf(path,sizeof(path),"%s/%s.part",ZHAOXI_PROMPT_DIR,id);
    FILE *file=fopen(path,"wb");if(!file)return -errno;
    int tty=open(ZHAOXI_PROMPT_CONSOLE,O_RDWR|O_NONBLOCK),rc=0;
    struct termios saved,raw;bool changed=false;
    if(tty<0){rc=-errno;goto done;}
    if(ioctl(tty,TCGETS,(unsigned long)&saved)){rc=-errno;goto done;}
    raw=saved;raw.c_iflag=0;raw.c_oflag=0;raw.c_lflag=0;
    raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if(ioctl(tty,TCSETS,(unsigned long)&raw)){rc=-errno;goto done;}
    changed=true;
    printf("PROMPT_RX_READY bytes=%lu block=1024\n",(unsigned long)length);fflush(stdout);
    int64_t deadline=milliseconds()+90000;
    unsigned char buffer[1024];size_t received=0;
    while(received<length) {
        size_t count=length-received;if(count>sizeof(buffer))count=sizeof(buffer);
        size_t have=0;
        while(have<count) {
            int64_t remaining=deadline-milliseconds();
            if(remaining<=0){rc=-ETIMEDOUT;goto done;}
            struct pollfd p={.fd=tty,.events=POLLIN};
            int ready=poll(&p,1,remaining<5000?(int)remaining:5000);
            if(ready<0 && errno==EINTR)continue;
            if(ready<=0 || !(p.revents&POLLIN)){rc=ready<0?-errno:-ETIMEDOUT;goto done;}
            ssize_t n=read(tty,buffer+have,count-have);
            if(n<0 && (errno==EAGAIN || errno==EINTR))continue;
            if(n<=0){rc=n<0?-errno:-EIO;goto done;}
            have+=(size_t)n;
        }
        if(fwrite(buffer,1,count,file)!=count){rc=-EIO;goto done;}
        received+=count;
        printf("PROMPT_RX_ACK %lu\n",(unsigned long)received);fflush(stdout);
    }
    if(fflush(file) || fsync(fileno(file))){rc=-errno;goto done;}
    if(fclose(file)){file=NULL;rc=-errno;goto done;}file=NULL;
    rc=zhaoxi_prompt_commit(id);
done:
    if(file)fclose(file);
    if(rc)unlink(path);
    if(changed && ioctl(tty,TCSETS,(unsigned long)&saved) && !rc)rc=-errno;
    if(tty>=0)close(tty);
    printf("\nPROMPT_RX_RESULT rc=%d\n",rc);fflush(stdout);
    return rc;
}

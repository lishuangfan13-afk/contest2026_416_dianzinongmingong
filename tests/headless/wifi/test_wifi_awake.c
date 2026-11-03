/* SPDX-License-Identifier: Apache-2.0 */
#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#define CONFIG_EXAMPLES_ZHAOXI 1
#define CONFIG_ZHAOXI_BES_AUDIO 1
#define CONFIG_NETUTILS_PPPD 1
static int64_t clock_ms=1000;
static int opens,closes,sets,fail_socket,fail_set;
static struct ifaddrs fixture;
static struct sockaddr_in ipv4;
static const char *TAG="fixture";
static char s_ip_str[INET_ADDRSTRLEN];
static int fake_clock(clockid_t id,struct timespec *out)
{assert(id==CLOCK_MONOTONIC);out->tv_sec=clock_ms/1000;out->tv_nsec=clock_ms%1000*1000000;return 0;}
static int fake_socket(int domain,int type,int protocol)
{assert(domain==AF_INET && type==SOCK_DGRAM && protocol==0);opens++;return fail_socket?-1:3;}
static int fake_close(int fd){assert(fd==3);closes++;return 0;}
static int fake_power(int fd,const char *name,bool on)
{assert(fd==3 && !strcmp(name,"wlan0") && !on);sets++;return fail_set?-1:0;}
static int fake_getifaddrs(struct ifaddrs **out){*out=&fixture;return 0;}
static void fake_freeifaddrs(struct ifaddrs *out){assert(out==&fixture);}
#define clock_gettime fake_clock
#define socket fake_socket
#define close fake_close
#define wapi_set_power_save fake_power
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#include "wifi_awake_actual.c"
static bool station(const char *name,unsigned flags)
{
    ipv4.sin_family=AF_INET;assert(inet_pton(AF_INET,"10.1.2.3",&ipv4.sin_addr)==1);
    fixture=(struct ifaddrs){.ifa_name=(char *)name,.ifa_flags=flags,.ifa_addr=(struct sockaddr *)&ipv4};
    return network_is_connected();
}
int main(void)
{
    unsigned ready=IFF_UP|IFF_RUNNING;
    assert(station("wlan0",ready) && sets==1 && opens==closes);
    assert(station("wlan0",ready) && sets==1);
    clock_ms+=61000;assert(station("wlan0",ready) && sets==2);
    assert(!station("wlan0",0));assert(station("wlan0",ready) && sets==3);
    int before=sets;assert(station("ppp0",ready) && sets==before);assert(!station("wlan1",ready) && sets==before);
    fail_set=1;assert(station("wlan0",ready));before=sets;
    clock_ms+=1000;assert(station("wlan0",ready) && sets==before);
    clock_ms+=30000;fail_set=0;assert(station("wlan0",ready) && sets==before+1);
    assert(opens==closes);
    assert(!station("wlan0",0));fail_socket=1;before=sets;
    assert(station("wlan0",ready) && sets==before && opens==closes+1);
    fail_socket=0;clock_ms+=31000;assert(station("wlan0",ready) && sets==before+1);
    before=sets;clock_ms+=61000;pthread_mutex_lock(&s_wifi_awake_lock);
    assert(station("wlan0",ready) && sets==before);pthread_mutex_unlock(&s_wifi_awake_lock);
    assert(station("wlan0",ready) && sets==before+1);
    puts("PASS: real Wi-Fi keep-awake policy; boot/reconnect refresh; retry rate limits; socket cleanup; PPP/SoftAP exclusion; nonblocking concurrent callers");
    return 0;
}

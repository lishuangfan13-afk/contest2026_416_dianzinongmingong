
#define _DEFAULT_SOURCE
#define CONFIG_EXAMPLES_ZHAOXI 1
#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
static struct ifaddrs fixture;
static struct sockaddr_in ipv4;
static int fail, frees;
static const char *TAG="test";
static char s_ip_str[INET_ADDRSTRLEN];
static int fake_getifaddrs(struct ifaddrs **at) {
  if (fail) return -1;
  *at=&fixture; return 0;
}
static void fake_freeifaddrs(struct ifaddrs *at) { assert(at == &fixture); frees++; }
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs

#include "station_readiness.c"

static void test(const char *name, const char *ip, unsigned flags, bool expected) {
  ipv4.sin_family=AF_INET;
  assert(inet_pton(AF_INET,ip,&ipv4.sin_addr)==1);
  fixture=(struct ifaddrs){.ifa_name=(char *)name,.ifa_flags=flags,
      .ifa_addr=(struct sockaddr *)&ipv4};
  int before=frees;
  assert(network_is_connected()==expected);
  assert(frees==before+1);
}
int main(void) {
  test("wlan0","0.0.0.0",IFF_UP|IFF_RUNNING,false);
  test("wlan0","10.1.2.3",0,false);
  test("wlan0","10.1.2.3",IFF_UP,false);
  test("wlan0","10.1.2.3",IFF_RUNNING,false);
  test("wlan1","192.168.4.1",IFF_UP|IFF_RUNNING,false);
  test("eth0","10.1.2.3",IFF_UP|IFF_RUNNING,false);
  test("lo","127.0.0.1",IFF_UP|IFF_RUNNING,false);
  test("wlan0","127.0.0.1",IFF_UP|IFF_RUNNING,false);
  test("wlan0","10.1.2.3",IFF_UP|IFF_RUNNING,true);
  fail=1; assert(!network_is_connected());
  puts("PASS: station-only readiness, stale/down IP rejection, SoftAP exclusion, loopback/zero IP rejection and getifaddrs failure. Fixture only.");
}

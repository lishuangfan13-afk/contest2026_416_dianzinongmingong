/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/timers/rtc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

extern struct rtc_lowerhalf_s *bes_rtc_lowerhalf(void);

int zhaoxi_clock_init(void)
{
    /* This BSP registers /dev/rtc0, but does not attach its lower half to
     * NuttX's generic up_rtc_* wrapper. Bind the existing driver once; do
     * not initialize or register the hardware a second time. */
    struct rtc_lowerhalf_s *lower = bes_rtc_lowerhalf();
    if (!lower) return -ENODEV;
    up_rtc_set_lowerhalf(lower, false);
    if (time(NULL) >= 1735689600) return 0;
    struct tm calendar;
    memset(&calendar, 0, sizeof(calendar));
    int result = up_rtc_getdatetime(&calendar);
    if (result) return result;
    if (calendar.tm_year < 125 || calendar.tm_year > 200) return -ENODATA;
    time_t epoch = timegm(&calendar);
    if (epoch < 1735689600) return -ENODATA;
    struct timeval value = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&value, NULL)) return -errno;
    printf("ZHAOXI RTC: restored UTC epoch=%lld\n", (long long)epoch);
    return 0;
}

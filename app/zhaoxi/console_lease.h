/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZHAOXI_CONSOLE_LEASE_H
#define ZHAOXI_CONSOLE_LEASE_H

/* Cooperative ownership of /dev/console; no NSH task suspension/cancellation.
 * Link console_lease.c as a strong object alongside the patched readline.
 * claim BEFORE saving/changing termios or starting PPP. On every exit, stop
 * and join PPP, restore termios, then release on the claiming thread. Do not
 * release while PPP can still read, or before successful TTY restoration.
 * APIs return 0 or negative errno, never modify the caller's TTY themselves.
 * claim is non-recursive: concurrent/nested claims fail with -EBUSY. Its
 * monotonic deadline is 250 ms (plus scheduler latency), not a blocking mutex.
 * A reader owns poll+one-byte read for at most a 50 ms idle poll interval.
 * The console driver must honor poll readiness for a one-byte read; all
 * competing console consumers must participate in this lease or be PPP.
 */
int zhaoxi_console_claim(void);
int zhaoxi_console_release(void);

/* Weak-product-hook ABI used by patches/0074. begin: 0 = unrelated fd
 * (original readline behavior), 1 = gate held, -EAGAIN = sleep/retry,
 * other negative errno = stop. Call end exactly once for every result 1.
 * Do not cancel a thread between begin/end; no blocking operation except the
 * bounded read poll belongs inside the gate. Echo uses nonblocking poll.
 * Both hooks must be present; readline ignores a partial pair.
 */
int zhaoxi_console_readline_begin(int fd);
void zhaoxi_console_readline_end(void);

#endif

/* src/compat/random.c — OS entropy backend (Layer 3: detect + wrap).
 *
 * Compiled WITHOUT _POSIX_C_SOURCE (COMPAT_CFLAGS/COMPAT_CPPFLAGS in the
 * Makefile) so the platform RNG is in the visible namespace:
 *
 *   - arc4random_buf  (BSD, macOS) — never fails, needs no fd or init.
 *   - getrandom       (Linux >= 3.17, glibc >= 2.25) — blocks until the
 *                     pool is initialized; we loop over short reads.
 *
 * ./configure selects exactly one via -DHAVE_*; the #error guards
 * against a misconfigured build silently producing a weak token.
 */
#include "random.h"

#if HAVE_ARC4RANDOM

#include <stdlib.h>

void unas_random_bytes(void *buf, size_t n)
{
    arc4random_buf(buf, n);
}

#elif HAVE_GETRANDOM

#include <sys/random.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

void unas_random_bytes(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = getrandom(p + off, n - off, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("unas: getrandom");
            abort();
        }
        off += (size_t)r;
    }
}

#else
#error "no entropy backend (HAVE_ARC4RANDOM/HAVE_GETRANDOM); re-run ./configure"
#endif

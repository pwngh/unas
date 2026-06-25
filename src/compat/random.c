/*
 * @pwngh/unas
 *
 * Copyright (c) Preston Neal
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE.md file in the root directory of this source tree.
 *
 * @license MIT
 */

/* src/compat/random.c — OS entropy backend (Layer 3: detect + wrap).
 *
 * Compiled with COMPAT_CPPFLAGS (config.mk): no _POSIX_C_SOURCE, plus
 * _DEFAULT_SOURCE so the platform RNG is visible — glibc otherwise hides
 * arc4random_buf/getrandom behind __USE_MISC under -std=c99:
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

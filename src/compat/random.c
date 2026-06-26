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
 * Built with COMPAT_CPPFLAGS (config.mk): no _POSIX_C_SOURCE, plus
 * _DEFAULT_SOURCE. Here's why that matters. Asking for strict C99
 * (-std=c99) makes glibc set a "standards-only" flag (__STRICT_ANSI__)
 * that hides anything outside the standard — including the OS random
 * functions, which sit behind glibc's __USE_MISC group. Defining
 * _DEFAULT_SOURCE re-opens that group, so the platform RNG becomes
 * visible to this file:
 *
 *   - arc4random_buf  (BSD, macOS) — never fails, needs no file handle
 *                     or warm-up.
 *   - getrandom       (Linux >= 3.17, glibc >= 2.25) — waits (blocks)
 *                     until the kernel's randomness pool is seeded, then
 *                     may hand back fewer bytes than asked, so we loop.
 *
 * ./configure picks exactly one at build time via -DHAVE_*; the #error
 * below stops a misconfigured build from quietly compiling with no
 * backend and minting a guessable token.
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

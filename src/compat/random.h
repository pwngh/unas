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

/* src/compat/random.h — portable cryptographic entropy.
 *
 * The ONE interface unas uses for randomness (minting the bearer token
 * when one is not supplied). The implementation in random.c is the only
 * translation unit that touches a non-POSIX RNG, and is compiled WITHOUT
 * _POSIX_C_SOURCE (see ./configure and the Makefile) so the platform
 * primitive is visible. Callers stay strictly POSIX.
 */
#ifndef UNAS_COMPAT_RANDOM_H
#define UNAS_COMPAT_RANDOM_H

#include <stddef.h>

/* Fill `buf` with `n` cryptographically strong random bytes -- the kind
 * safe to build a secret from, not the predictable kind. If the random
 * source ever fails badly, this function does not return at all: it aborts
 * the whole program on purpose. A guessable access token would be worse
 * than a crash, so it refuses to hand back anything less than real
 * randomness. */
void unas_random_bytes(void *buf, size_t n);

#endif /* UNAS_COMPAT_RANDOM_H */

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

/* src/core/net.h — TCP listener. C99 + POSIX.1-2008.
 *
 * A tiny wrapper over getaddrinfo/socket/bind/listen so unasd can bind a
 * stream socket on loopback or the LAN without caring about IPv4 vs IPv6.
 * No TLS: unasd speaks plaintext HTTP/1.1 by design (TLS is a proxy's
 * job). Access control is the bearer token, not the OS.
 */
#ifndef UNAS_NET_H
#define UNAS_NET_H

#include <stddef.h>

/* Create, bind, and listen on a TCP socket at host:port.
 *   host  NULL/"" -> all interfaces; else e.g. "127.0.0.1", "0.0.0.0", "::1".
 *   port  decimal string; "0" -> an ephemeral port.
 * On success returns the listening fd and writes the actually-bound port
 * to *out_port (if non-NULL). On failure returns -1 with `err` set. */
int net_listen(const char *host, const char *port, int *out_port,
               char *err, size_t errn);

/* Accept one connection (retries on EINTR). Returns the fd or -1. */
int net_accept(int listen_fd);

#endif /* UNAS_NET_H */

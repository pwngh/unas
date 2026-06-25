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

/* src/core/net.c — TCP listener. Strict C99 + POSIX.1-2008. */
#include "net.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static void seterr(char *err, size_t errn, const char *what)
{
    if (err && errn) snprintf(err, errn, "%s: %s", what, strerror(errno));
}

int net_listen(const char *host, const char *port, int *out_port,
               char *err, size_t errn)
{
    struct addrinfo hints, *res = NULL, *ai;
    int fd = -1, rc, yes = 1;

    memset(&hints, 0, sizeof hints);
    /* AF_UNSPEC + AI_PASSIVE + NULL node (host NULL/"") yields both v4 and
     * v6 wildcard candidates to try in turn; an explicit host narrows it. */
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    rc = getaddrinfo((host && *host) ? host : NULL, port, &hints, &res);
    if (rc != 0) {
        /* getaddrinfo's codes are not errno: format with gai_strerror, not
         * seterr()/strerror, or the message is garbage. */
        if (err && errn) snprintf(err, errn, "getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        /* Best-effort (failure is ignored): lets a restart re-bind the port
         * while a previous socket lingers in TIME_WAIT. */
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    if (fd < 0) { seterr(err, errn, "bind"); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);

    if (listen(fd, 64) != 0) { seterr(err, errn, "listen"); close(fd); return -1; }

    if (out_port) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        *out_port = 0;
        if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
            if (ss.ss_family == AF_INET)
                *out_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            else if (ss.ss_family == AF_INET6)
                *out_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
        }
    }
    return fd;
}

int net_accept(int listen_fd)
{
    int fd;
    do { fd = accept(listen_fd, NULL, NULL); } while (fd < 0 && errno == EINTR);
    return fd;
}

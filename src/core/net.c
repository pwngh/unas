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
    hints.ai_family   = AF_UNSPEC;       /* v4 or v6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    rc = getaddrinfo((host && *host) ? host : NULL, port, &hints, &res);
    if (rc != 0) {
        if (err && errn) snprintf(err, errn, "getaddrinfo: %s", gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
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

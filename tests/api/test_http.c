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

/* tests/api/test_http.c — the request parser: request line, query strip,
 * case-insensitive headers, Content-Length, and the buffered body prefix.
 * Feeds raw bytes through a socketpair. Links libunas.a. */
#include "http.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static int fails = 0;

static void expect_str(const char *what, const char *got, const char *want)
{
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s = %s (want %s)\n", what, got ? got : "(null)", want);
        fails++;
    }
}

int main(void)
{
    int sv[2];
    http_conn c;
    http_request r;
    char err[128];
    const char *pre;
    size_t pren;

    const char *req =
        "PUT /v1/fs/a%20b.txt?v=1 HTTP/1.1\r\n"
        "Host: unas.local\r\n"
        "Content-Length: 5\r\n"
        "Authorization: Bearer s3cr3t\r\n"
        "Destination: /v1/fs/c.txt\r\n"
        "\r\n"
        "hello";

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        fprintf(stderr, "FAIL: socketpair\n"); return 1;
    }
    if (write(sv[1], req, strlen(req)) < 0) { fprintf(stderr, "FAIL: write\n"); return 1; }

    http_conn_init(&c, sv[0]);
    if (http_read_request(&c, &r, err, sizeof err) != 0) {
        fprintf(stderr, "FAIL: http_read_request: %s\n", err); return 1;
    }

    expect_str("method", r.method, "PUT");
    expect_str("target", r.target, "/v1/fs/a%20b.txt?v=1");   /* query kept in target */
    expect_str("path",   r.path,   "/v1/fs/a%20b.txt");        /* query stripped */
    if (r.content_length != 5) { fprintf(stderr, "FAIL: content_length=%lld (want 5)\n", r.content_length); fails++; }

    /* header lookup is case-insensitive */
    expect_str("content-length", http_header_get(&r, "content-length"), "5");
    expect_str("AUTHORIZATION",  http_header_get(&r, "AUTHORIZATION"), "Bearer s3cr3t");
    expect_str("Destination",    http_header_get(&r, "Destination"), "/v1/fs/c.txt");
    if (http_header_get(&r, "X-Absent") != NULL) { fprintf(stderr, "FAIL: absent header returned non-NULL\n"); fails++; }

    /* the 5 body bytes are buffered, ready to stream */
    http_body_prefix(&c, &pre, &pren);
    if (pren != 5 || memcmp(pre, "hello", 5) != 0) {
        fprintf(stderr, "FAIL: body prefix len=%lu\n", (unsigned long)pren); fails++;
    }

    close(sv[0]); close(sv[1]);

    /* a request bearing two Content-Length headers must be rejected (-> 400) */
    {
        int sv2[2];
        http_conn c2;
        http_request r2;
        char err2[128];
        const char *dup =
            "PUT /v1/fs/x HTTP/1.1\r\n"
            "Content-Length: 5\r\n"
            "Content-Length: 6\r\n"
            "\r\n"
            "hello";
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) != 0) { fprintf(stderr, "FAIL: socketpair2\n"); return 1; }
        if (write(sv2[1], dup, strlen(dup)) < 0) { fprintf(stderr, "FAIL: write2\n"); return 1; }
        http_conn_init(&c2, sv2[0]);
        if (http_read_request(&c2, &r2, err2, sizeof err2) != -1) {
            fprintf(stderr, "FAIL: duplicate Content-Length accepted (want reject)\n"); fails++;
        }
        close(sv2[0]); close(sv2[1]);
    }

    /* parser error surface: malformed / truncated heads are rejected,
     * a junk (no-colon) header line is ignored rather than fatal. */
    {
        struct { const char *name; const char *raw; int want; } cases[] = {
            { "empty-request-line", "\r\n\r\n",                          -1 },
            { "truncated-head",     "GET /x HTTP/1.1\r\n",               -1 },  /* no blank line, then EOF */
            { "no-colon-header-ok", "GET /x HTTP/1.1\r\njunk\r\n\r\n",    0 },
            { "malformed-clen",     "PUT /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n", -1 },
            { "negative-clen",      "PUT /x HTTP/1.1\r\nContent-Length: -5\r\n\r\n",  -1 },
        };
        size_t i;
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            int sp[2];
            http_conn cc;
            http_request rr;
            char e[128];
            int rc;
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { fprintf(stderr, "FAIL: socketpair3\n"); return 1; }
            if (write(sp[1], cases[i].raw, strlen(cases[i].raw)) < 0) { fprintf(stderr, "FAIL: write3\n"); return 1; }
            close(sp[1]);                       /* EOF after the bytes */
            http_conn_init(&cc, sp[0]);
            rc = http_read_request(&cc, &rr, e, sizeof e);
            if (rc != cases[i].want) { fprintf(stderr, "FAIL: %s rc=%d want %d\n", cases[i].name, rc, cases[i].want); fails++; }
            close(sp[0]);
        }
    }

    if (fails) { fprintf(stderr, "test_http: %d failure(s)\n", fails); return 1; }
    printf("test_http: OK\n");
    return 0;
}

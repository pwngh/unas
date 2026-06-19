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
    if (fails) { fprintf(stderr, "test_http: %d failure(s)\n", fails); return 1; }
    printf("test_http: OK\n");
    return 0;
}

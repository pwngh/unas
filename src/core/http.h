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

/* src/core/http.h — minimal HTTP/1.1 request parser + response writer.
 *
 * Just enough HTTP to be a clean file API: parse the request line and
 * headers into a fixed struct, expose the body as a stream (the bytes
 * already buffered past the headers, plus whatever remains on the fd),
 * and write responses. No chunked decoding, no keep-alive — one request
 * per connection, every response carries `Connection: close`. Aside from
 * the prefix that shares the head's read (see http_body_prefix), file-body
 * bytes never touch this layer's buffers; the caller streams the rest
 * to/from disk.
 *
 * Strict C99 + POSIX.1-2008.
 */
#ifndef UNAS_HTTP_H
#define UNAS_HTTP_H

#include <stddef.h>

#define HTTP_BUF_SZ        65536
#define HTTP_MAX_HEADERS   48
#define HTTP_MAX_HDR_NAME  64
#define HTTP_MAX_HDR_VALUE 2048
#define HTTP_MAX_TARGET    4096
#define HTTP_MAX_METHOD    16

typedef struct {
    char name[HTTP_MAX_HDR_NAME];
    char value[HTTP_MAX_HDR_VALUE];
} http_header;

/* A connection plus its read buffer. The slice `buf[pos..len)` is the
 * bytes we've read from the socket but not yet handed out; once
 * http_read_request returns, that leftover slice is the front of the
 * request body (the headers having been consumed). */
typedef struct {
    int    fd;
    char   buf[HTTP_BUF_SZ];
    size_t len;
    size_t pos;
} http_conn;

typedef struct {
    char        method[HTTP_MAX_METHOD];
    char        target[HTTP_MAX_TARGET];   /* raw request-target (path?query) */
    char        path[HTTP_MAX_TARGET];     /* path only (query stripped) */
    http_header headers[HTTP_MAX_HEADERS];
    size_t      nheaders;
    long long   content_length;            /* -1 if absent or unparseable */
} http_request;

void http_conn_init(http_conn *c, int fd);

/* Read + parse the request line and headers. Returns 0 on success,
 * 1 on a clean EOF before any bytes (idle close), -1 on a malformed or
 * oversized head (err set). Body bytes remain buffered in `c`. */
int http_read_request(http_conn *c, http_request *r, char *err, size_t errn);

/* Case-insensitive header lookup; NULL if absent. */
const char *http_header_get(const http_request *r, const char *name);

/* Hands back the body bytes that happened to come in alongside the
 * headers and are already sitting in the connection buffer. The pointer
 * doesn't own that memory -- it just points into `c`'s buffer, so it's
 * only good until the next read on `c` overwrites it. Whatever body is
 * left after this prefix (Content-Length minus this many bytes) the caller
 * reads straight from the fd. */
void http_body_prefix(http_conn *c, const char **p, size_t *n);

/* Write exactly n bytes to fd (handles short writes / EINTR). 0 / -1. */
int http_write_all(int fd, const void *buf, size_t n);

/* Reason phrase for a status code (e.g. 404 -> "Not Found"). */
const char *http_reason(int code);

/* Full small response: status line, optional Content-Type, computed
 * Content-Length, optional extra header lines (each "Name: value\r\n"),
 * `Connection: close`, then the body. content_type/extra/body may be
 * NULL. */
void http_send(http_conn *c, int code, const char *reason,
               const char *content_type, const char *extra_headers,
               const void *body, size_t body_len);

/* Status line + caller-supplied header block + `Connection: close` and
 * the blank line. The caller then streams the body via http_write_all.
 * `extra_headers` must include Content-Type/Content-Length itself. */
void http_send_headers(http_conn *c, int code, const char *reason,
                       const char *extra_headers);

/* application/json convenience (computes length, reason from the code). */
void http_send_json(http_conn *c, int code, const char *json);

#endif /* UNAS_HTTP_H */

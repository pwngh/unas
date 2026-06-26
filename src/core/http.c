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

/* src/core/http.c — HTTP/1.1 request parse + response writer. C99/POSIX. */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <errno.h>
#include <unistd.h>

/* We'd like the compiler to catch printf-style mistakes — like passing an
 * int where the format string says %s — in our one varargs helper below.
 * Standard C99 has no portable way to ask for that, but GCC and Clang offer
 * a __attribute__((format(printf,...))) hint. This macro carries that hint
 * where the compiler understands it (so -Wformat checks the format string
 * against the values passed for it) and expands to nothing everywhere else. */
#if defined(__GNUC__) || defined(__clang__)
#  define UNAS_PRINTF(fmt_idx, var_idx) __attribute__((format(printf, fmt_idx, var_idx)))
#else
#  define UNAS_PRINTF(fmt_idx, var_idx)
#endif

/* ====================================================================
 * Low-level I/O
 * ==================================================================== */

void http_conn_init(http_conn *c, int fd)
{
    c->fd = fd;
    c->len = 0;
    c->pos = 0;
}

int http_write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

/* ====================================================================
 * Request parsing
 * ==================================================================== */

static void copy_bounded(char *dst, size_t dstsz, const char *src, size_t srclen)
{
    if (srclen >= dstsz) srclen = dstsz - 1;
    memcpy(dst, src, srclen);
    dst[srclen] = '\0';
}

/* Find CRLF in buf[start..end); return its index, or `end` if absent. */
static size_t find_crlf(const char *buf, size_t start, size_t end)
{
    size_t i;
    for (i = start; i + 1 < end; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n') return i;
    return end;
}

static void parse_request_line(http_request *r, const char *line, size_t n)
{
    size_t i = 0, ms, ts, te;
    /* method */
    ms = i;
    while (i < n && line[i] != ' ') i++;
    copy_bounded(r->method, sizeof r->method, line + ms, i - ms);
    while (i < n && line[i] == ' ') i++;
    /* target */
    ts = i;
    while (i < n && line[i] != ' ') i++;
    te = i;
    copy_bounded(r->target, sizeof r->target, line + ts, te - ts);
    /* The third word on the request line is the client's HTTP version
     * ("HTTP/1.1", "HTTP/1.0", ...). We don't even read it: this server
     * always replies in HTTP/1.1 and then hangs up, so whatever version
     * the client claims can't change a single byte of our answer. */
    /* path = target up to '?' */
    {
        const char *q = memchr(r->target, '?', strlen(r->target));
        size_t plen = q ? (size_t)(q - r->target) : strlen(r->target);
        copy_bounded(r->path, sizeof r->path, r->target, plen);
    }
}

static void parse_header_line(http_request *r, const char *line, size_t n)
{
    size_t colon = 0, vs;
    if (r->nheaders >= HTTP_MAX_HEADERS) return;
    while (colon < n && line[colon] != ':') colon++;
    if (colon >= n) return;                 /* no colon: ignore junk line */
    vs = colon + 1;
    while (vs < n && (line[vs] == ' ' || line[vs] == '\t')) vs++;
    {
        http_header *h = &r->headers[r->nheaders];
        size_t vlen = n - vs;
        /* trim trailing whitespace */
        while (vlen > 0 && (line[vs + vlen - 1] == ' ' || line[vs + vlen - 1] == '\t'))
            vlen--;
        copy_bounded(h->name, sizeof h->name, line, colon);
        copy_bounded(h->value, sizeof h->value, line + vs, vlen);
        r->nheaders++;
        if (strcasecmp(h->name, "Content-Length") == 0) {
            char *endp;
            long long v = strtoll(h->value, &endp, 10);
            if (endp != h->value && *endp == '\0' && v >= 0) r->content_length = v;
        }
    }
}

int http_read_request(http_conn *c, http_request *r, char *err, size_t errn)
{
    size_t hdr_end = 0;
    int found = 0;

    memset(r, 0, sizeof *r);
    r->content_length = -1;

    /* Read until the CRLFCRLF that terminates the head. */
    for (;;) {
        size_t i;
        for (i = 0; i + 3 < c->len; i++) {
            if (c->buf[i] == '\r' && c->buf[i + 1] == '\n' &&
                c->buf[i + 2] == '\r' && c->buf[i + 3] == '\n') { hdr_end = i; found = 1; break; }
        }
        if (found) break;
        if (c->len >= sizeof c->buf - 1) {
            if (err && errn) snprintf(err, errn, "request head too large");
            return -1;
        }
        {
            ssize_t got;
            do { got = read(c->fd, c->buf + c->len, sizeof c->buf - c->len); }
            while (got < 0 && errno == EINTR);
            if (got < 0) { if (err && errn) snprintf(err, errn, "read: %s", strerror(errno)); return -1; }
            if (got == 0) {
                if (c->len == 0) return 1;          /* clean idle close */
                if (err && errn) snprintf(err, errn, "truncated head");
                return -1;
            }
            c->len += (size_t)got;
        }
    }

    /* Split the head [0..hdr_end) into the request line + header lines. */
    {
        size_t start = 0, lineno = 0;
        while (start < hdr_end) {
            size_t crlf = find_crlf(c->buf, start, hdr_end);
            size_t linelen = crlf - start;
            if (lineno == 0) parse_request_line(r, c->buf + start, linelen);
            else             parse_header_line(r, c->buf + start, linelen);
            lineno++;
            if (crlf >= hdr_end) break;
            start = crlf + 2;
        }
    }

    if (r->method[0] == '\0' || r->path[0] == '\0') {
        if (err && errn) snprintf(err, errn, "malformed request line");
        return -1;
    }

    /* If a request carries two Content-Length headers, their numbers can
     * disagree about how long the body is. An attacker exploits that gap
     * to sneak a second hidden request through a proxy that trusts one
     * value while we trust the other ("request smuggling"). With no safe
     * way to pick a winner, we refuse: RFC 7230 §3.3.2 allows a 400 here,
     * and serve_conn sends it. */
    {
        size_t i, ncl = 0;
        for (i = 0; i < r->nheaders; i++)
            if (strcasecmp(r->headers[i].name, "Content-Length") == 0) ncl++;
        if (ncl > 1) {
            if (err && errn) snprintf(err, errn, "duplicate Content-Length");
            return -1;
        }
    }

    /* parse_header_line leaves content_length at -1 in two cases: the header
     * was absent, or it was present but garbage (non-numeric or negative).
     * Here we tell those apart by checking whether the header exists at all.
     * If it does and the value is still -1, the client DID send a length, it
     * was just unreadable — so we reject with 400 instead of letting it fall
     * through to the 411 "Content-Length required" path, which would wrongly
     * scold the client for omitting something it actually sent. RFC 7230
     * §3.3.2 calls for rejecting an invalid Content-Length. */
    if (http_header_get(r, "Content-Length") && r->content_length < 0) {
        if (err && errn) snprintf(err, errn, "malformed Content-Length");
        return -1;
    }

    c->pos = hdr_end + 4;                    /* body begins here */
    return 0;
}

const char *http_header_get(const http_request *r, const char *name)
{
    size_t i;
    for (i = 0; i < r->nheaders; i++)
        if (strcasecmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    return NULL;
}

void http_body_prefix(http_conn *c, const char **p, size_t *n)
{
    *p = c->buf + c->pos;
    *n = c->len - c->pos;
}

/* ====================================================================
 * Response writing
 * ==================================================================== */

const char *http_reason(int code)
{
    switch (code) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 507: return "Insufficient Storage";
        default:  return "Status";
    }
}

/* Bounded append into buf; clamps rather than overflowing. */
static void apnd(char *buf, size_t cap, size_t *off, const char *fmt, ...)
    UNAS_PRINTF(4, 5);

static void apnd(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*off >= cap) return;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *off += (size_t)n;
    if (*off >= cap) *off = cap - 1;
}

void http_send(http_conn *c, int code, const char *reason,
               const char *content_type, const char *extra_headers,
               const void *body, size_t body_len)
{
    char head[1024];
    size_t off = 0;
    apnd(head, sizeof head, &off, "HTTP/1.1 %d %s\r\n", code,
         reason ? reason : http_reason(code));
    if (content_type) apnd(head, sizeof head, &off, "Content-Type: %s\r\n", content_type);
    apnd(head, sizeof head, &off, "Content-Length: %lu\r\n", (unsigned long)body_len);
    if (extra_headers) apnd(head, sizeof head, &off, "%s", extra_headers);
    apnd(head, sizeof head, &off, "Connection: close\r\n\r\n");
    (void)http_write_all(c->fd, head, off);
    if (body && body_len) (void)http_write_all(c->fd, body, body_len);
}

void http_send_headers(http_conn *c, int code, const char *reason,
                       const char *extra_headers)
{
    char head[1024];
    size_t off = 0;
    apnd(head, sizeof head, &off, "HTTP/1.1 %d %s\r\n", code,
         reason ? reason : http_reason(code));
    if (extra_headers) apnd(head, sizeof head, &off, "%s", extra_headers);
    apnd(head, sizeof head, &off, "Connection: close\r\n\r\n");
    (void)http_write_all(c->fd, head, off);
}

void http_send_json(http_conn *c, int code, const char *json)
{
    http_send(c, code, http_reason(code), "application/json; charset=utf-8",
              NULL, json, strlen(json));
}

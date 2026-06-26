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

/* src/core/jsonw.c — JSON builder. Strict C99. */
#include "jsonw.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ====================================================================
 * Builder
 * ==================================================================== */

void jb_init(json_buf *b)
{
    memset(b, 0, sizeof *b);
}

void jb_free(json_buf *b)
{
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* Ensure room for `extra` more bytes plus the trailing NUL, growing the buffer
 * geometrically (doubling from 64). b->err is a sticky failure latch: once an
 * allocation fails it stays set, every later jb_* call short-circuits to a
 * no-op, and jb_take returns NULL — so a caller builds the whole document and
 * checks for failure once at the end, never after each append. */
static int jb_reserve(json_buf *b, size_t extra)
{
    size_t need;
    if (b->err) return 0;
    need = b->len + extra + 1;            /* +1 for NUL */
    if (need <= b->cap) return 1;
    {
        size_t ncap = b->cap ? b->cap : 64;
        char  *nb;
        while (ncap < need) ncap *= 2;
        nb = (char *)realloc(b->buf, ncap);
        if (!nb) { b->err = 1; return 0; }
        b->buf = nb;
        b->cap = ncap;
    }
    return 1;
}

static void jb_putc(json_buf *b, char c)
{
    if (!jb_reserve(b, 1)) return;
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}

static void jb_puts(json_buf *b, const char *s)
{
    size_t n = strlen(s);
    if (!jb_reserve(b, n)) return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* Hand the finished buffer to the caller, who now owns it and must free(),
 * and reset the builder to empty so the json_buf can be reused or dropped.
 * NULL is returned only on err; a builder that emitted nothing still yields a
 * valid empty C-string ("") via the reserve-and-NUL path below, never NULL. */
char *jb_take(json_buf *b, size_t *len)
{
    char *out;
    if (b->err) { jb_free(b); return NULL; }
    if (!b->buf) { (void)jb_reserve(b, 0); if (b->err) return NULL; b->buf[0] = '\0'; }
    out = b->buf;
    if (len) *len = b->len;
    b->buf = NULL; b->len = b->cap = 0;
    return out;
}

/* Separator/nesting bookkeeping, so callers emit values without tracking
 * commas. need_comma[depth] means "this container already holds a value": so
 * pre_value() inserts the leading ',' before the next one and post_value()
 * sets the flag once a value lands. push()/pop() keep need_comma per nesting
 * level, so closing a container restores the parent's state. suppress_comma is
 * the lone exception — a value right after jb_key() must not add its own ','
 * (the key already wrote the separator and the colon), so jb_key sets it to
 * skip exactly the next pre_value(). */
static void pre_value(json_buf *b)
{
    if (b->suppress_comma) { b->suppress_comma = 0; return; }
    if (b->depth > 0 && b->need_comma[b->depth]) jb_putc(b, ',');
}

static void post_value(json_buf *b)
{
    if (b->depth > 0) b->need_comma[b->depth] = 1;
}

static void push(json_buf *b)
{
    if (b->depth >= JB_MAX_DEPTH) { b->err = 1; return; }
    b->depth++;
    b->need_comma[b->depth] = 0;
}

static void pop(json_buf *b)
{
    if (b->depth > 0) b->depth--;
    post_value(b);
}

void jb_obj_open(json_buf *b) { pre_value(b); jb_putc(b, '{'); push(b); }
void jb_obj_close(json_buf *b) { jb_putc(b, '}'); pop(b); }
void jb_arr_open(json_buf *b) { pre_value(b); jb_putc(b, '['); push(b); }
void jb_arr_close(json_buf *b) { jb_putc(b, ']'); pop(b); }

/* How many bytes (1-4) the UTF-8 character at s occupies, or 0 if s[0]
 * isn't a valid start byte. UTF-8 packs one character into 1-4 bytes; a
 * multi-byte one begins with a "start" byte and is filled out by
 * "continuation" bytes (each tagged 10xxxxxx). We reject three kinds of
 * malformed input that an attacker could otherwise smuggle through:
 *   - overlong encodings: a character padded into more bytes than it needs,
 *     so two different byte strings could mean the same character (a way to
 *     sneak a '/' or '.' past a path check),
 *   - surrogates (U+D800..U+DFFF): half-of-a-pair code points that are legal
 *     in UTF-16 but forbidden as standalone UTF-8 characters,
 *   - anything above U+10FFFF, the highest code point Unicode defines.
 * We peek ahead at the continuation bytes to validate. That peek is safe
 * because the string ends in a NUL (0) byte, and 0 can never be a
 * continuation byte: the moment we hit it a check fails and we stop, so we
 * never read past the end of the string. */
static int utf8_seq_len(const unsigned char *s)
{
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) {                      /* 110xxxxx: 2 bytes */
        if (c < 0xC2) return 0;                    /* overlong */
        if ((s[1] & 0xC0) != 0x80) return 0;
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {                      /* 1110xxxx: 3 bytes */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0;     /* overlong */
        if (c == 0xED && s[1] >= 0xA0) return 0;    /* surrogate half */
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {                      /* 11110xxx: 4 bytes */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0;     /* overlong */
        if (c > 0xF4 || (c == 0xF4 && s[1] >= 0x90)) return 0;  /* > U+10FFFF */
        return 4;
    }
    return 0;
}

/* Emit a JSON string literal: wrap s in quotes and escape what JSON
 * requires. Valid UTF-8 is copied through unchanged; a byte that isn't
 * valid UTF-8 is replaced with U+FFFD, the standard "unknown character"
 * symbol. That swap matters because a filename can hold arbitrary bytes,
 * and one bad byte left raw would corrupt the whole response into
 * something no JSON parser would accept. */
static void emit_string(json_buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    jb_putc(b, '"');
    while (*p) {
        unsigned char c = *p;
        switch (c) {
            case '"':  jb_puts(b, "\\\""); p++; break;
            case '\\': jb_puts(b, "\\\\"); p++; break;
            case '\b': jb_puts(b, "\\b");  p++; break;
            case '\f': jb_puts(b, "\\f");  p++; break;
            case '\n': jb_puts(b, "\\n");  p++; break;
            case '\r': jb_puts(b, "\\r");  p++; break;
            case '\t': jb_puts(b, "\\t");  p++; break;
            default:
                if (c < 0x20) {
                    char u[7];
                    snprintf(u, sizeof u, "\\u%04x", (unsigned)c);
                    jb_puts(b, u);
                    p++;
                } else if (c < 0x80) {
                    jb_putc(b, (char)c);            /* ASCII */
                    p++;
                } else {
                    int n = utf8_seq_len(p), i;
                    if (n == 0) { jb_puts(b, "\\ufffd"); p++; }   /* invalid byte */
                    else { for (i = 0; i < n; i++) jb_putc(b, (char)p[i]); p += n; }
                }
        }
    }
    jb_putc(b, '"');
}

void jb_key(json_buf *b, const char *key)
{
    if (b->depth > 0 && b->need_comma[b->depth]) jb_putc(b, ',');
    emit_string(b, key);
    jb_putc(b, ':');
    b->suppress_comma = 1;     /* the value that follows must not add ',' */
}

void jb_str(json_buf *b, const char *s) { pre_value(b); emit_string(b, s); post_value(b); }

void jb_int(json_buf *b, long long v)
{
    char num[32];
    pre_value(b);
    snprintf(num, sizeof num, "%lld", v);
    jb_puts(b, num);
    post_value(b);
}

void jb_bool(json_buf *b, int v) { pre_value(b); jb_puts(b, v ? "true" : "false"); post_value(b); }
void jb_null(json_buf *b)        { pre_value(b); jb_puts(b, "null"); post_value(b); }

void jb_kv_str(json_buf *b, const char *k, const char *v) { jb_key(b, k); jb_str(b, v); }
void jb_kv_int(json_buf *b, const char *k, long long v)   { jb_key(b, k); jb_int(b, v); }

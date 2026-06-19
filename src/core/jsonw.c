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

/* Emit a JSON string literal (quotes + escaping). */
static void emit_string(json_buf *b, const char *s)
{
    jb_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  jb_puts(b, "\\\""); break;
            case '\\': jb_puts(b, "\\\\"); break;
            case '\b': jb_puts(b, "\\b");  break;
            case '\f': jb_puts(b, "\\f");  break;
            case '\n': jb_puts(b, "\\n");  break;
            case '\r': jb_puts(b, "\\r");  break;
            case '\t': jb_puts(b, "\\t");  break;
            default:
                if (c < 0x20) {
                    char u[7];
                    snprintf(u, sizeof u, "\\u%04x", (unsigned)c);
                    jb_puts(b, u);
                } else {
                    jb_putc(b, (char)c);   /* pass UTF-8 bytes through */
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

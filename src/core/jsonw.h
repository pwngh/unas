/* src/core/jsonw.h — unas's small JSON builder.
 *
 * Used only for the JSON envelope (listings, /v1/status, /v1/shares, and
 * error bodies). File bytes never touch it — reads and writes stream raw.
 * Emit-only by design: unas builds JSON responses but never parses JSON
 * (no request body is JSON in v1). Keeping the builder in the tree avoids
 * a third-party runtime dependency — itself a portability win.
 *
 * Strict C99.
 */
#ifndef UNAS_JSONW_H
#define UNAS_JSONW_H

#include <stddef.h>

/* ====================================================================
 * Builder — emit JSON into a growable buffer.
 *
 * Comma and quoting bookkeeping is automatic. Usage:
 *
 *     json_buf b; jb_init(&b);
 *     jb_obj_open(&b);
 *       jb_kv_str(&b, "code", "ENOENT");
 *       jb_kv_int(&b, "http", 404);
 *     jb_obj_close(&b);
 *     size_t n; char *s = jb_take(&b, &n);   // caller owns s
 *
 * On allocation failure b.err is set; all further calls are no-ops and
 * jb_take returns NULL. Always check b.err (or a NULL from jb_take).
 * ==================================================================== */

#define JB_MAX_DEPTH 32

typedef struct {
    char         *buf;
    size_t        len;
    size_t        cap;
    int           err;                 /* nonzero after an allocation failure */
    int           depth;
    int           suppress_comma;      /* set by jb_key; next value skips ',' */
    unsigned char need_comma[JB_MAX_DEPTH + 1];
} json_buf;

void  jb_init(json_buf *b);
void  jb_free(json_buf *b);                 /* free internal buffer */
char *jb_take(json_buf *b, size_t *len);    /* hand buffer to caller; NULL on err */

void jb_obj_open(json_buf *b);
void jb_obj_close(json_buf *b);
void jb_arr_open(json_buf *b);
void jb_arr_close(json_buf *b);
void jb_key(json_buf *b, const char *key);

void jb_str(json_buf *b, const char *s);    /* escaped string value */
void jb_int(json_buf *b, long long v);
void jb_bool(json_buf *b, int v);
void jb_null(json_buf *b);

void jb_kv_str(json_buf *b, const char *k, const char *v);
void jb_kv_int(json_buf *b, const char *k, long long v);

#endif /* UNAS_JSONW_H */

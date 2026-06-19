/* src/core/unasd.c — the unas daemon: jail a share root, serve the HTTP
 * file API over TCP. Strict C99 + POSIX.1-2008.
 *
 * Shape: bind a listener, fork one child per connection (SIGCHLD ignored
 * so children auto-reap), parse one request, run the router, close. The
 * router is a single function over the fsapi syscall wrappers. See
 * README.md for the surface.
 */
#include "http.h"
#include "net.h"
#include "fsapi.h"
#include "jsonw.h"
#include "random.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define PATHCAP 4096

/* The whole per-process configuration, gathered once by main and passed
 * by const pointer down the request path. Nothing lives in file-scope
 * mutable state: every handler reads what it needs from this argument,
 * so each has no hidden inputs and can be reasoned about — and tested —
 * in isolation. Forked children inherit a read-only snapshot. */
typedef struct {
    const char     *root;     /* normalized share root; the jail boundary    */
    const char     *token;    /* bearer token; required on all routes but /healthz */
    const char     *addr;     /* bind address, echoed by /v1/status          */
    int             port;     /* bound TCP port (resolved when 0 was asked)   */
    struct timespec start;    /* CLOCK_MONOTONIC at startup, for uptime_s     */
    const char     *version;  /* server version string                        */
} unas_server;

/* ====================================================================
 * Small helpers
 * ==================================================================== */

/* Compare a presented token against the configured one in time that does
 * not depend on how many leading bytes match, so a network attacker
 * cannot recover the token byte-by-byte from response timing. The loop
 * runs over the configured token's full length regardless of input. */
static bool token_eq(const char *got, const char *want)
{
    size_t lw = strlen(want), lg = strlen(got), i;
    volatile unsigned char diff = (unsigned char)(lg ^ lw);
    for (i = 0; i < lw; i++)
        diff |= (unsigned char)((unsigned char)want[i] ^
                                (unsigned char)(i < lg ? got[i] : 0));
    return diff == 0;
}

static const char *guess_ctype(const char *path)
{
    const char *d = strrchr(path, '.');
    if (!d) return "application/octet-stream";
    if (!strcasecmp(d, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcasecmp(d, ".html") || !strcasecmp(d, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(d, ".json")) return "application/json";
    if (!strcasecmp(d, ".css"))  return "text/css";
    if (!strcasecmp(d, ".js"))   return "text/javascript";
    if (!strcasecmp(d, ".png"))  return "image/png";
    if (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(d, ".gif"))  return "image/gif";
    if (!strcasecmp(d, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(d, ".pdf"))  return "application/pdf";
    if (!strcasecmp(d, ".mp4"))  return "video/mp4";
    if (!strcasecmp(d, ".zip"))  return "application/zip";
    return "application/octet-stream";
}

/* ====================================================================
 * errno -> HTTP status + name (the closed error set from the README).
 * One table so the status and the symbol can never drift apart.
 * ==================================================================== */

static const struct { int e; int http; const char *name; } ERRMAP[] = {
    { ENOENT,       404, "ENOENT" },
    { EACCES,       403, "EACCES" },
    { EPERM,        403, "EPERM" },
    { EROFS,        403, "EROFS" },
    { EEXIST,       409, "EEXIST" },
    { ENOTEMPTY,    409, "ENOTEMPTY" },
    { EISDIR,       409, "EISDIR" },
    { ENOTDIR,      409, "ENOTDIR" },
    { EINVAL,       400, "EINVAL" },
    { ENAMETOOLONG, 414, "ENAMETOOLONG" },
    { ENOSPC,       507, "ENOSPC" },
#ifdef EDQUOT
    { EDQUOT,       507, "EDQUOT" },
#endif
};

static int err_http(int e)
{
    size_t i;
    for (i = 0; i < sizeof ERRMAP / sizeof *ERRMAP; i++)
        if (ERRMAP[i].e == e) return ERRMAP[i].http;
    return 500;
}

static const char *err_name(int e)
{
    size_t i;
    for (i = 0; i < sizeof ERRMAP / sizeof *ERRMAP; i++)
        if (ERRMAP[i].e == e) return ERRMAP[i].name;
    return "EIO";
}

/* Build the one error-envelope shape: {"error":{code,http[,path],message}}.
 * Returns a freshly malloc'd string (caller frees), or NULL on OOM. `path`
 * is the URL subpath the client asked for — never the server's filesystem
 * path, which must not leak. */
static char *problem_json(int code, const char *name, const char *path, const char *msg)
{
    json_buf b;
    jb_init(&b);
    jb_obj_open(&b);
    jb_key(&b, "error");
    jb_obj_open(&b);
    jb_kv_str(&b, "code", name);
    jb_kv_int(&b, "http", (long long)code);
    if (path) jb_kv_str(&b, "path", path);
    jb_kv_str(&b, "message", msg);
    jb_obj_close(&b);
    jb_obj_close(&b);
    return jb_take(&b, NULL);
}

static void reply_problem(http_conn *c, int code, const char *name,
                          const char *path, const char *msg)
{
    char *s = problem_json(code, name, path, msg);
    if (s) { http_send_json(c, code, s); free(s); }
    else http_send(c, 500, "Internal Server Error",
                   "text/plain; charset=utf-8", NULL, "oom\n", 4);
}

static void reply_errno(http_conn *c, int e, const char *path)
{
    reply_problem(c, err_http(e), err_name(e), path, strerror(e));
}

static void reply_auth(http_conn *c)
{
    const char *body =
        "{\"error\":{\"code\":\"EAUTH\",\"http\":401,"
        "\"message\":\"missing or invalid bearer token\"}}";
    http_send(c, 401, "Unauthorized", "application/json; charset=utf-8",
              "WWW-Authenticate: Bearer\r\n", body, strlen(body));
}

static void reply_405(http_conn *c, const char *path)
{
    reply_problem(c, 405, "EMETHOD", path, "unsupported method");
}

/* HEAD on a JSON endpoint: status + content-type, empty body. */
static void send_json_head(http_conn *c)
{
    http_send(c, 200, "OK", "application/json; charset=utf-8", NULL, NULL, 0);
}

/* ====================================================================
 * GET/HEAD on a file (with Range)
 * ==================================================================== */

/* Parse "bytes=a-b" / "a-" / "-suffix". 0 if satisfiable, -1 if not — a
 * non-numeric token is rejected (-> 416), not silently read as 0. */
static int parse_range(const char *h, long long size, long long *ps, long long *pe)
{
    long long s, e;
    char *endp;
    if (strncmp(h, "bytes=", 6) != 0) return -1;
    h += 6;
    if (*h == '-') {                       /* suffix: last N bytes */
        long long n = strtoll(h + 1, &endp, 10);
        if (endp == h + 1 || *endp != '\0') return -1;   /* digits only */
        if (n <= 0 || size == 0) return -1;
        if (n > size) n = size;
        *ps = size - n; *pe = size - 1; return 0;
    }
    s = strtoll(h, &endp, 10);
    if (endp == h || *endp != '-') return -1;            /* "<digits>-" required */
    if (endp[1] == '\0') {
        e = size - 1;
    } else {
        e = strtoll(endp + 1, &endp, 10);
        if (*endp != '\0') return -1;                    /* trailing garbage */
    }
    if (s < 0 || s >= size) return -1;
    if (e >= size) e = size - 1;
    if (e < s) return -1;
    *ps = s; *pe = e; return 0;
}

static void serve_file(http_conn *c, const char *abspath, const struct stat *st,
                       bool is_head, const char *range, const char *sub)
{
    char tbare[64], etag[80], lm[40], crange[96], hdrs[640], buf[65536];
    long long size, start = 0, end, clen;
    bool partial = false;
    int fd;

    fd = open(abspath, O_RDONLY);
    if (fd < 0) { reply_errno(c, errno, sub); return; }

    size = (long long)st->st_size;
    end = size - 1;
    if (range) {
        if (parse_range(range, size, &start, &end) == 0) {
            partial = true;
        } else {                            /* present but unsatisfiable */
            char h[96], *body;
            snprintf(h, sizeof h, "Content-Range: bytes */%lld\r\n", size);
            (void)close(fd);
            body = problem_json(416, "ERANGE", sub, "range not satisfiable");
            if (body) {
                http_send(c, 416, NULL, "application/json; charset=utf-8", h, body, strlen(body));
                free(body);
            } else {
                http_send(c, 416, NULL, "application/json; charset=utf-8", h, NULL, 0);
            }
            return;
        }
    }
    clen = partial ? (end - start + 1) : size;

    fsapi_fmt_etag(size, (long long)st->st_mtime, tbare, sizeof tbare);
    snprintf(etag, sizeof etag, "\"%s\"", tbare);
    fsapi_fmt_httpdate((long long)st->st_mtime, lm, sizeof lm);
    crange[0] = '\0';
    if (partial)
        snprintf(crange, sizeof crange, "Content-Range: bytes %lld-%lld/%lld\r\n",
                 start, end, size);
    snprintf(hdrs, sizeof hdrs,
             "Content-Type: %s\r\nContent-Length: %lld\r\nETag: %s\r\n"
             "Last-Modified: %s\r\nAccept-Ranges: bytes\r\n%s",
             guess_ctype(abspath), clen, etag, lm, crange);

    http_send_headers(c, partial ? 206 : 200, NULL, hdrs);
    if (is_head) { (void)close(fd); return; }
    if (partial && lseek(fd, (off_t)start, SEEK_SET) == (off_t)-1) { (void)close(fd); return; }

    {
        long long remaining = clen;
        while (remaining > 0) {
            size_t want = (remaining < (long long)sizeof buf) ? (size_t)remaining : sizeof buf;
            ssize_t r = read(fd, buf, want);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;
            if (http_write_all(c->fd, buf, (size_t)r) != 0) break;
            remaining -= r;
        }
    }
    (void)close(fd);
}

/* ====================================================================
 * Conditional-request + MOVE/COPY helpers
 * ==================================================================== */

static bool if_match_ok(const struct stat *st, const char *im)
{
    char bare[64], want[80];
    size_t i = 0, o = 0;
    fsapi_fmt_etag((long long)st->st_size, (long long)st->st_mtime, bare, sizeof bare);
    if (im[0] == 'W' && im[1] == '/') im += 2;        /* weak-validator prefix */
    while (im[i]) { char ch = im[i++]; if (ch == '"') continue; if (o + 1 < sizeof want) want[o++] = ch; }
    want[o] = '\0';
    return strcmp(bare, want) == 0;
}

static bool depth_infinity(const http_request *r)
{
    const char *d = http_header_get(r, "Depth");
    return d && strcasecmp(d, "infinity") == 0;
}

/* Extract the "/v1/fs..." subpath from a Destination header value (which
 * may be an absolute URL or an absolute path). NULL if it isn't one. */
static const char *dest_subpath(const char *dst)
{
    const char *p = dst, *scheme, *f;
    scheme = strstr(dst, "://");
    if (scheme) { p = strchr(scheme + 3, '/'); if (!p) return NULL; }
    f = strstr(p, "/v1/fs");
    if (!f) return NULL;
    f += 6;
    if (f[0] != '/' && f[0] != '\0') return NULL;
    return f;
}

/* ====================================================================
 * /v1/fs dispatch — the file verbs
 * ==================================================================== */

static void dispatch_fs(const unas_server *srv, http_conn *c, http_request *r,
                        const char *sub, const char *abspath, bool is_dir)
{
    const char *m = r->method;
    const char *disp = sub[0] ? sub : "/";
    struct stat st;
    bool have;
    int sterr;

    errno = 0;
    have = (stat(abspath, &st) == 0);
    sterr = errno;

    if (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0) {
        bool head = (m[0] == 'H');
        if (!have) { reply_errno(c, sterr ? sterr : ENOENT, disp); return; }
        if (is_dir) {
            char *js = NULL;
            if (!S_ISDIR(st.st_mode)) { reply_problem(c, 409, "ENOTDIR", disp, "not a directory"); return; }
            if (head) { send_json_head(c); return; }
            if (fsapi_list_json(abspath, disp, &js) != 0) { reply_errno(c, errno, disp); return; }
            http_send_json(c, 200, js); free(js); return;
        }
        if (S_ISDIR(st.st_mode)) { reply_problem(c, 409, "EISDIR", disp, "is a directory; append a trailing slash to list"); return; }
        if (!S_ISREG(st.st_mode)) { reply_problem(c, 409, "EINVAL", disp, "not a regular file"); return; }
        serve_file(c, abspath, &st, head, http_header_get(r, "Range"), disp);
        return;
    }

    if (strcmp(m, "PUT") == 0) {
        char loc[PATHCAP + 32];
        if (is_dir) {
            if (have && S_ISDIR(st.st_mode)) { http_send(c, 204, "No Content", NULL, NULL, NULL, 0); return; }
            if (have)                        { reply_problem(c, 409, "ENOTDIR", disp, "path exists and is not a directory"); return; }
            if (fsapi_mkdirs(abspath) != 0)  { reply_errno(c, errno, disp); return; }
            snprintf(loc, sizeof loc, "Location: /v1/fs%s\r\n", sub);
            http_send(c, 201, "Created", NULL, loc, NULL, 0);
            return;
        }
        if (have && S_ISDIR(st.st_mode)) { reply_problem(c, 409, "EISDIR", disp, "path is a directory"); return; }
        {
            const char *inm = http_header_get(r, "If-None-Match");
            const char *im  = http_header_get(r, "If-Match");
            if (inm && strcmp(inm, "*") == 0 && have) { reply_problem(c, 412, "EEXIST", disp, "exists (If-None-Match: *)"); return; }
            if (im && (!have || !if_match_ok(&st, im))) { reply_problem(c, 412, "EMISMATCH", disp, "ETag does not match (If-Match)"); return; }
        }
        if (r->content_length < 0) { reply_problem(c, 411, "ELENGTH", disp, "Content-Length required"); return; }
        {
            const char *ex = http_header_get(r, "Expect");
            if (ex && strcasecmp(ex, "100-continue") == 0)
                (void)http_write_all(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
        }
        {
            const char *pre; size_t pren;
            http_body_prefix(c, &pre, &pren);
            if (fsapi_write_stream(abspath, c->fd, pre, pren, r->content_length) != 0) {
                reply_errno(c, errno, disp); return;
            }
        }
        snprintf(loc, sizeof loc, "Location: /v1/fs%s\r\n", sub);
        http_send(c, have ? 200 : 201, have ? "OK" : "Created", NULL, loc, NULL, 0);
        return;
    }

    if (strcmp(m, "DELETE") == 0) {
        const char *im = http_header_get(r, "If-Match");
        if (!have) { reply_errno(c, sterr ? sterr : ENOENT, disp); return; }
        if (im && !if_match_ok(&st, im)) { reply_problem(c, 412, "EMISMATCH", disp, "ETag does not match (If-Match)"); return; }
        if (is_dir) {
            if (!S_ISDIR(st.st_mode)) { reply_problem(c, 409, "ENOTDIR", disp, "not a directory"); return; }
            if (fsapi_rmtree(abspath, depth_infinity(r)) != 0) { reply_errno(c, errno, disp); return; }
        } else {
            if (S_ISDIR(st.st_mode)) { reply_problem(c, 409, "EISDIR", disp, "append a trailing slash to remove a directory"); return; }
            if (unlink(abspath) != 0) { reply_errno(c, errno, disp); return; }
        }
        http_send(c, 204, "No Content", NULL, NULL, NULL, 0);
        return;
    }

    if (strcmp(m, "MOVE") == 0 || strcmp(m, "COPY") == 0) {
        const char *dst = http_header_get(r, "Destination");
        const char *dsub;
        char dabs[PATHCAP], loc[PATHCAP + 32];
        bool ddir = false;
        if (!dst) { reply_problem(c, 400, "EINVAL", disp, "Destination header required"); return; }
        dsub = dest_subpath(dst);
        if (!dsub) { reply_problem(c, 400, "EINVAL", disp, "Destination must target /v1/fs/..."); return; }
        if (fsapi_resolve(srv->root, dsub, dabs, sizeof dabs, &ddir) != 0) { reply_errno(c, errno, dsub); return; }
        if (!have) { reply_problem(c, 404, "ENOENT", disp, "source not found"); return; }
        {
            const char *im = http_header_get(r, "If-Match");
            if (im && !if_match_ok(&st, im)) { reply_problem(c, 412, "EMISMATCH", disp, "ETag does not match (If-Match)"); return; }
        }
        if (strcmp(m, "MOVE") == 0) { if (fsapi_move(abspath, dabs) != 0) { reply_errno(c, errno, disp); return; } }
        else                        { if (fsapi_copy(abspath, dabs) != 0) { reply_errno(c, errno, disp); return; } }
        snprintf(loc, sizeof loc, "Location: /v1/fs%s\r\n", dsub);
        http_send(c, 201, "Created", NULL, loc, NULL, 0);
        return;
    }

    reply_405(c, disp);
}

/* ====================================================================
 * /v1/status and /v1/shares
 * ==================================================================== */

static void reply_status(const unas_server *srv, http_conn *c, bool is_head)
{
    struct stat st;
    struct timespec now;
    json_buf b;
    char *s;
    bool mounted, writable;

    if (is_head) { send_json_head(c); return; }

    mounted  = (stat(srv->root, &st) == 0 && S_ISDIR(st.st_mode));
    writable = (mounted && access(srv->root, W_OK) == 0);
    now.tv_sec = 0; now.tv_nsec = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);

    jb_init(&b);
    jb_obj_open(&b);
    jb_kv_str(&b, "version", srv->version);
    jb_kv_str(&b, "addr", srv->addr);
    jb_kv_int(&b, "port", (long long)srv->port);
    jb_kv_str(&b, "root", srv->root);
    jb_key(&b, "mounted");  jb_bool(&b, mounted);
    jb_key(&b, "writable"); jb_bool(&b, writable);
    jb_kv_int(&b, "uptime_s", (long long)(now.tv_sec - srv->start.tv_sec));
    jb_obj_close(&b);
    s = jb_take(&b, NULL);
    if (s) { http_send_json(c, 200, s); free(s); }
    else reply_problem(c, 500, "EIO", NULL, "oom");
}

static void reply_shares(const unas_server *srv, http_conn *c, bool is_head)
{
    struct statvfs vfs;
    json_buf b;
    char *s;
    const char *name;

    if (is_head) { send_json_head(c); return; }

    name = strrchr(srv->root, '/');
    name = (name && name[1]) ? name + 1 : srv->root;

    jb_init(&b);
    jb_obj_open(&b);
    jb_key(&b, "shares");
    jb_arr_open(&b);
    jb_obj_open(&b);
    jb_kv_str(&b, "name", name);
    jb_kv_str(&b, "path", srv->root);
    if (statvfs(srv->root, &vfs) == 0) {
        unsigned long long fr = vfs.f_frsize ? (unsigned long long)vfs.f_frsize
                                             : (unsigned long long)vfs.f_bsize;
        jb_kv_int(&b, "total_bytes", (long long)(fr * (unsigned long long)vfs.f_blocks));
        jb_kv_int(&b, "free_bytes",  (long long)(fr * (unsigned long long)vfs.f_bfree));
        jb_kv_int(&b, "avail_bytes", (long long)(fr * (unsigned long long)vfs.f_bavail));
        /* A UNAS export reports the storage pool, not this share's
         * quota, so these figures are pool-wide; the scope field marks
         * them as such rather than as the share's own free space. */
        jb_kv_str(&b, "scope", "pool");
    } else {
        jb_key(&b, "total_bytes"); jb_null(&b);
        jb_kv_str(&b, "scope", "unknown");
    }
    jb_obj_close(&b);
    jb_arr_close(&b);
    jb_obj_close(&b);
    s = jb_take(&b, NULL);
    if (s) { http_send_json(c, 200, s); free(s); }
    else reply_problem(c, 500, "EIO", NULL, "oom");
}

/* ====================================================================
 * The router
 * ==================================================================== */

static bool authed(const unas_server *srv, const http_request *r)
{
    const char *h = http_header_get(r, "Authorization");
    if (!h || strncmp(h, "Bearer ", 7) != 0) return false;
    return token_eq(h + 7, srv->token);
}

static void handle_request(const unas_server *srv, http_conn *c, http_request *r)
{
    if (strcmp(r->path, "/healthz") == 0) {
        http_send(c, 200, "OK", "text/plain; charset=utf-8", NULL, "ok\n", 3);
        return;
    }
    if (!authed(srv, r)) { reply_auth(c); return; }

    if (strcmp(r->path, "/v1/status") == 0) {
        if (strcmp(r->method, "GET") == 0 || strcmp(r->method, "HEAD") == 0)
            reply_status(srv, c, r->method[0] == 'H');
        else reply_405(c, r->path);
        return;
    }
    if (strcmp(r->path, "/v1/shares") == 0) {
        if (strcmp(r->method, "GET") == 0 || strcmp(r->method, "HEAD") == 0)
            reply_shares(srv, c, r->method[0] == 'H');
        else reply_405(c, r->path);
        return;
    }
    if (strncmp(r->path, "/v1/fs", 6) == 0 && (r->path[6] == '\0' || r->path[6] == '/')) {
        const char *sub = r->path + 6;
        char abspath[PATHCAP];
        bool is_dir = false;
        if (fsapi_resolve(srv->root, sub, abspath, sizeof abspath, &is_dir) != 0) {
            reply_errno(c, errno, sub[0] ? sub : "/");
            return;
        }
        dispatch_fs(srv, c, r, sub, abspath, is_dir);
        return;
    }
    reply_problem(c, 404, "ENOENT", r->path, "no such endpoint");
}

static void serve_conn(const unas_server *srv, int fd)
{
    http_conn c;
    http_request r;
    char err[160];
    int rc;
    http_conn_init(&c, fd);
    rc = http_read_request(&c, &r, err, sizeof err);
    if (rc == 1) return;                                  /* idle close */
    if (rc < 0) {
        http_send(&c, 400, "Bad Request", "text/plain; charset=utf-8",
                  NULL, "bad request\n", 12);
        return;
    }
    handle_request(srv, &c, &r);
}

/* ====================================================================
 * Startup
 * ==================================================================== */

static void gen_token(char *out, size_t outn)
{
    static const char *hx = "0123456789abcdef";
    unsigned char rb[16];
    size_t i;
    if (outn < 33) { if (outn) out[0] = '\0'; return; }
    unas_random_bytes(rb, sizeof rb);
    for (i = 0; i < 16; i++) { out[2 * i] = hx[rb[i] >> 4]; out[2 * i + 1] = hx[rb[i] & 0x0f]; }
    out[32] = '\0';
}

static int read_token_file(const char *path, char *out, size_t outn)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f) return -1;
    if (!fgets(out, (int)outn, f)) { fclose(f); return -1; }
    fclose(f);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' '  || out[n - 1] == '\t'))
        out[--n] = '\0';
    return out[0] ? 0 : -1;
}

/* Best-effort: drop the chosen port and token where a UI can read them. */
static void write_state(const char *dir, int port, const char *token)
{
    char p[PATHCAP], line[300];
    int fd, n;
    snprintf(p, sizeof p, "%s/unas.port", dir);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { n = snprintf(line, sizeof line, "%d\n", port); if (n > 0) (void)http_write_all(fd, line, (size_t)n); close(fd); }
    snprintf(p, sizeof p, "%s/unas.token", dir);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { n = snprintf(line, sizeof line, "%s\n", token); if (n > 0) (void)http_write_all(fd, line, (size_t)n); close(fd); }
}

static void strip_trailing_slashes(char *s)
{
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') s[--n] = '\0';
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--addr HOST] [--port N] [--token TOK | --token-file F] <share-root>\n"
        "  env: UNAS_ROOT UNAS_ADDR UNAS_PORT UNAS_TOKEN UNAS_TOKEN_FILE UNAS_STATE\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *addr = getenv("UNAS_ADDR");
    const char *port = getenv("UNAS_PORT");
    const char *token = getenv("UNAS_TOKEN");
    const char *tokenfile = getenv("UNAS_TOKEN_FILE");
    const char *root = getenv("UNAS_ROOT");
    const char *state = getenv("UNAS_STATE");
    char root_buf[PATHCAP], token_buf[256];
    char err[256], nerr[256];
    unas_server srv;
    int lfd, bound = 0, i;
    struct sigaction sa;

    if (!addr || !*addr) addr = "127.0.0.1";
    if (!port || !*port) port = "8088";

    for (i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) addr = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) token = argv[++i];
        else if (strcmp(argv[i], "--token-file") == 0 && i + 1 < argc) tokenfile = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "unas: unknown option %s\n", argv[i]); usage(argv[0]); return 2; }
        else root = argv[i];
    }
    if (!root) { usage(argv[0]); return 2; }

    snprintf(root_buf, sizeof root_buf, "%s", root);
    strip_trailing_slashes(root_buf);

    if (tokenfile) {
        if (read_token_file(tokenfile, token_buf, sizeof token_buf) != 0) {
            fprintf(stderr, "unas: cannot read token file %s\n", tokenfile); return 1;
        }
    } else if (token && *token) {
        snprintf(token_buf, sizeof token_buf, "%s", token);
    } else {
        gen_token(token_buf, sizeof token_buf);
    }
    if (token_buf[0] == '\0') { fprintf(stderr, "unas: empty token\n"); return 2; }

    if (fsapi_check_root(root_buf, err, sizeof err) != 0) {
        fprintf(stderr, "unas: %s\n", err); return 1;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);          /* children auto-reap (POSIX) */

    lfd = net_listen(addr, port, &bound, nerr, sizeof nerr);
    if (lfd < 0) { fprintf(stderr, "unas: %s\n", nerr); return 1; }

    /* Configuration is complete and immutable from here on. */
    srv.root    = root_buf;
    srv.token   = token_buf;
    srv.addr    = addr;
    srv.port    = bound;
    srv.version = "unas/1.0";
    srv.start.tv_sec = 0; srv.start.tv_nsec = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &srv.start);

    if (state && *state) write_state(state, bound, srv.token);

    fprintf(stderr, "unasd: serving %s on %s:%d\n", srv.root, addr, bound);
    printf("PORT=%d\nTOKEN=%s\n", bound, srv.token);
    fflush(stdout);

    for (;;) {
        int cfd = net_accept(lfd);
        pid_t pid;
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        pid = fork();
        if (pid < 0) { serve_conn(&srv, cfd); close(cfd); continue; }   /* degraded: inline */
        if (pid == 0) { close(lfd); serve_conn(&srv, cfd); close(cfd); _exit(0); }
        close(cfd);
    }
    close(lfd);
    return 0;
}

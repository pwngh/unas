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

/* src/core/fsapi.c — the jailed filesystem. Strict C99 + POSIX.1-2008.
 * _POSIX_C_SOURCE is supplied by the build (config.mk CPPFLAGS). */
#include "fsapi.h"
#include "jsonw.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FSAPI_PATH 4096
#define FSAPI_IOBUF 65536

/* ====================================================================
 * Small filesystem helpers
 *
 * The durability primitives. Each new file is written under a temporary
 * name next to its target rather than in a shared tmp/ directory, so the
 * publish step (rename(2), or link(2) for create-only writes) is always
 * within one filesystem and therefore atomic, wherever the target lands
 * under the share.
 * ==================================================================== */

/* write(2) may transfer fewer bytes than requested, and may be interrupted by
 * a signal before any land (EINTR); loop until all `n` are written or a real
 * error occurs. */
static int write_all_fd(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

static void fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    /* Some servers reject fsync on a directory fd with EINVAL; that is
     * not fatal — it just means dir-entry durability isn't separately
     * guaranteed. */
    (void)fsync(fd);
    (void)close(fd);
}

static void parent_of(const char *path, char *out, size_t outn)
{
    const char *slash = strrchr(path, '/');
    size_t len;
    if (!slash) { snprintf(out, outn, "."); return; }
    len = (size_t)(slash - path);
    if (len == 0) { snprintf(out, outn, "/"); return; }   /* parent of "/x" */
    if (len >= outn) len = outn - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

/* A temp name unique enough without coordination: pid + monotonic time + a
 * per-process counter (for two calls within the same nanosecond). It need not
 * be collision-proof — the write opens it with O_CREAT|O_EXCL, which fails
 * rather than overwrites on the rare clash, so a collision is a clean error,
 * never silent data loss. */
static void temp_path_in(const char *dir, char *out, size_t outn)
{
    static unsigned counter = 0;
    struct timespec ts;
    ts.tv_sec = 0; ts.tv_nsec = 0;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, outn, "%s/.unas.tmp.%ld.%ld.%ld.%u",
             dir, (long)getpid(), (long)ts.tv_sec, (long)ts.tv_nsec, counter++);
}

int fsapi_mkdirs(const char *abspath)
{
    char tmp[FSAPI_PATH];
    char *p;
    if (strlen(abspath) >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    snprintf(tmp, sizeof tmp, "%s", abspath);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ====================================================================
 * Path jail
 * ==================================================================== */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode `in` into `out`. A decoded NUL is refused (errno=EINVAL):
 * left in, it would terminate the C string early and hide the rest of the path
 * from every later check and syscall — a classic truncation bypass. Malformed
 * escapes are refused too; ENAMETOOLONG if `out` would overflow. */
static int url_decode(const char *in, char *out, size_t outn)
{
    size_t o = 0, i = 0;
    while (in[i]) {
        char ch;
        if (in[i] == '%') {
            int hi = hexval((unsigned char)in[i + 1]);
            int lo = (hi < 0) ? -1 : hexval((unsigned char)in[i + 2]);
            if (hi < 0 || lo < 0) { errno = EINVAL; return -1; }
            ch = (char)((hi << 4) | lo);
            if (ch == '\0') { errno = EINVAL; return -1; }
            i += 3;
        } else {
            ch = in[i++];
        }
        if (o + 1 >= outn) { errno = ENAMETOOLONG; return -1; }
        out[o++] = ch;
    }
    out[o] = '\0';
    return 0;
}

int fsapi_resolve(const char *root, const char *urlsub,
                  char *out, size_t outn, bool *is_dir)
{
    char dec[FSAPI_PATH];
    size_t i = 0, L, o, rootlen;
    bool dir;

    if (url_decode(urlsub, dec, sizeof dec) != 0) return -1;  /* errno set */
    L = strlen(dec);
    dir = (L > 0 && dec[L - 1] == '/');

    rootlen = strlen(root);
    if (rootlen >= outn) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, root, rootlen);
    o = rootlen;
    out[o] = '\0';

    /* The jail itself. Decoding ran first, so an encoded '/' or '..' can't slip
     * past as text; now walk segment by segment, keeping only clean ones —
     * empty and "." dropped, ".." refused, any control byte refused — and build
     * `out` as root + "/" + segment. The result is structurally always under
     * root, and it is rejected here, before any syscall: no open/stat/opendir
     * ever sees a path that escapes. (Symlinks that resolve out are a separate
     * concern, caught by fsapi_contained.) */
    while (i < L) {
        size_t start, seglen, k;
        while (i < L && dec[i] == '/') i++;
        start = i;
        while (i < L && dec[i] != '/') i++;
        seglen = i - start;
        if (seglen == 0) continue;
        if (seglen == 1 && dec[start] == '.') continue;
        if (seglen == 2 && dec[start] == '.' && dec[start + 1] == '.') { errno = EINVAL; return -1; }
        for (k = start; k < start + seglen; k++)
            if ((unsigned char)dec[k] < 0x20) { errno = EINVAL; return -1; }
        if (o + 1 + seglen >= outn) { errno = ENAMETOOLONG; return -1; }
        out[o++] = '/';
        memcpy(out + o, dec + start, seglen);
        o += seglen;
    }
    out[o] = '\0';
    if (is_dir) *is_dir = dir || (o == rootlen);
    return 0;
}

int fsapi_contained(const char *root, const char *abspath)
{
    char rootc[FSAPI_PATH], cur[FSAPI_PATH], res[FSAPI_PATH];
    size_t rl;

    if (!realpath(root, rootc)) { errno = EACCES; return -1; }
    rl = strlen(rootc);
    if (strlen(abspath) >= sizeof cur) { errno = ENAMETOOLONG; return -1; }
    snprintf(cur, sizeof cur, "%s", abspath);

    /* Walk up to the deepest component that exists; realpath resolves any
     * symlinks within it. A path that resolves outside `root` — or a symlink
     * loop / permission wall on the way — is refused. The not-yet-existing
     * tail carries no symlink of its own, so checking the prefix suffices. */
    for (;;) {
        char *slash;
        if (realpath(cur, res)) {
            size_t l = strlen(res);
            /* res must start with rootc and break on a '/' (so "/srv/sharefoo"
             * is not read as inside "/srv/share"). The rl>1 guard keeps this
             * correct should rootc ever be "/" (which has no trailing boundary
             * char); serving "/" is refused at startup (fsapi_check_root), so
             * the guard is belt-and-suspenders. */
            if (l < rl || strncmp(res, rootc, rl) != 0 ||
                (rl > 1 && res[rl] != '\0' && res[rl] != '/')) { errno = EACCES; return -1; }
            return 0;
        }
        if (errno != ENOENT && errno != ENOTDIR) { errno = EACCES; return -1; }
        slash = strrchr(cur, '/');
        if (!slash || slash == cur) { errno = EACCES; return -1; }
        *slash = '\0';
    }
}

int fsapi_check_root(const char *root, char *err, size_t errn)
{
    struct stat st;
    if (geteuid() == 0) {
        snprintf(err, errn, "refusing to run as root: the UNAS export squashes "
                 "root; run as a normal user whose uid/gid matches the share");
        return -1;
    }
    if (stat(root, &st) != 0) {
        snprintf(err, errn, "root '%s': %s", root, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(err, errn, "root '%s' is not a directory", root);
        return -1;
    }
    {
        /* Refuse to serve the filesystem root: a daemon whose job is to expose
         * one mounted share should never re-export all of "/" over HTTP. This
         * is the footgun's only legitimate place to fail, and it also keeps
         * fsapi_resolve/fsapi_contained off the "/" boundary edge entirely. */
        char rp[FSAPI_PATH];
        if (!realpath(root, rp)) {
            snprintf(err, errn, "root '%s': %s", root, strerror(errno));
            return -1;
        }
        if (strcmp(rp, "/") == 0) {
            snprintf(err, errn, "refusing to serve the filesystem root '/'; "
                     "point --root at a specific mounted share");
            return -1;
        }
    }
    return 0;
}

/* ====================================================================
 * Operations
 * ==================================================================== */

int fsapi_write_stream(const char *abspath, int src_fd,
                       const char *prebuf, size_t prelen,
                       long long content_length, bool excl)
{
    char parent[FSAPI_PATH], tmp[FSAPI_PATH], iobuf[FSAPI_IOBUF];
    int fd;
    long long remaining = content_length;
    size_t pre_use;

    parent_of(abspath, parent, sizeof parent);
    if (fsapi_mkdirs(parent) != 0) return -1;
    temp_path_in(parent, tmp, sizeof tmp);

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;

    pre_use = ((long long)prelen > remaining) ? (size_t)remaining : prelen;
    if (pre_use && write_all_fd(fd, prebuf, pre_use) != 0) goto fail;
    remaining -= (long long)pre_use;

    while (remaining > 0) {
        size_t want = (remaining < (long long)sizeof iobuf) ? (size_t)remaining : sizeof iobuf;
        ssize_t r = read(src_fd, iobuf, want);
        if (r < 0) { if (errno == EINTR) continue; goto fail; }
        if (r == 0) { errno = ECONNRESET; goto fail; }   /* client closed early */
        if (write_all_fd(fd, iobuf, (size_t)r) != 0) goto fail;
        remaining -= r;
    }
    if (fsync(fd) != 0) goto fail;
    if (close(fd) != 0) goto fail_noclose;   /* fd is spent; fail_noclose only unlinks */
    if (excl) {
        /* Atomic create-only: link fails EEXIST if abspath already exists,
         * leaving no window for a racing creator to slip in between. */
        if (link(tmp, abspath) != 0) goto fail_noclose;
        (void)unlink(tmp);
    } else {
        if (rename(tmp, abspath) != 0) goto fail_noclose;
    }
    fsync_dir(parent);
    return 0;

fail:
    { int e = errno; (void)close(fd); errno = e; }
fail_noclose:
    { int e = errno; (void)unlink(tmp); errno = e; }
    return -1;
}

int fsapi_move(const char *src, const char *dst)
{
    /* Same-filesystem only: this publishes with rename(2), so a cross-device
     * move returns EXDEV to the caller rather than copying. No internal
     * fallback: an API client wanting a cross-device move issues COPY then
     * DELETE. */
    char dparent[FSAPI_PATH], sparent[FSAPI_PATH];
    parent_of(dst, dparent, sizeof dparent);
    if (fsapi_mkdirs(dparent) != 0) return -1;
    if (rename(src, dst) != 0) return -1;
    fsync_dir(dparent);
    parent_of(src, sparent, sizeof sparent);
    fsync_dir(sparent);
    return 0;
}

int fsapi_copy(const char *src, const char *dst)
{
    char parent[FSAPI_PATH], tmp[FSAPI_PATH], iobuf[FSAPI_IOBUF];
    struct stat st;
    int in, out;
    ssize_t r;

    in = open(src, O_RDONLY);
    if (in < 0) return -1;
    if (fstat(in, &st) != 0) { int e = errno; (void)close(in); errno = e; return -1; }
    if (S_ISDIR(st.st_mode)) { (void)close(in); errno = EISDIR; return -1; }

    parent_of(dst, parent, sizeof parent);
    if (fsapi_mkdirs(parent) != 0) { int e = errno; (void)close(in); errno = e; return -1; }
    temp_path_in(parent, tmp, sizeof tmp);

    out = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) { int e = errno; (void)close(in); errno = e; return -1; }

    while ((r = read(in, iobuf, sizeof iobuf)) > 0) {
        if (write_all_fd(out, iobuf, (size_t)r) != 0) goto fail;
    }
    if (r < 0) goto fail;
    (void)close(in); in = -1;
    if (fsync(out) != 0) goto fail;
    if (close(out) != 0) goto fail_noclose;  /* out is spent; `in` already closed, so fail_noclose only unlinks here */
    if (rename(tmp, dst) != 0) goto fail_noclose;
    fsync_dir(parent);
    return 0;

fail:
    { int e = errno; (void)close(out); errno = e; }
fail_noclose:
    { int e = errno; if (in >= 0) (void)close(in); (void)unlink(tmp); errno = e; }
    return -1;
}

int fsapi_rmtree(const char *abspath, bool recursive)
{
    /* lstat, not stat: a symlink is removed as a file, never followed. This is
     * what keeps rmtree from descending through a link and deleting files
     * outside the requested subtree. */
    struct stat st;
    if (lstat(abspath, &st) != 0) return -1;

    if (!S_ISDIR(st.st_mode))
        return unlink(abspath);

    if (!recursive)
        return rmdir(abspath);

    {
        DIR *d = opendir(abspath);
        struct dirent *e;
        if (!d) return -1;
        while ((e = readdir(d)) != NULL) {
            char child[FSAPI_PATH];
            int n;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            n = snprintf(child, sizeof child, "%s/%s", abspath, e->d_name);
            if (n < 0 || (size_t)n >= sizeof child) { closedir(d); errno = ENAMETOOLONG; return -1; }
            if (fsapi_rmtree(child, 1) != 0) { int er = errno; closedir(d); errno = er; return -1; }
        }
        closedir(d);
    }
    return rmdir(abspath);
}

int fsapi_list_json(const char *abspath, const char *urlpath, char **out_json)
{
    DIR *d = opendir(abspath);
    struct dirent *e;
    json_buf b;
    char *s;

    if (!d) return -1;

    jb_init(&b);
    jb_obj_open(&b);
    jb_kv_str(&b, "path", urlpath);
    jb_key(&b, "entries");
    jb_arr_open(&b);

    while ((e = readdir(d)) != NULL) {
        char child[FSAPI_PATH], mt[40], et[64];
        struct stat st;
        int n;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n = snprintf(child, sizeof child, "%s/%s", abspath, e->d_name);
        if (n < 0 || (size_t)n >= sizeof child) continue;
        if (stat(child, &st) != 0) continue;             /* skip a racing unlink */

        jb_obj_open(&b);
        jb_kv_str(&b, "name", e->d_name);
        jb_kv_str(&b, "type", S_ISDIR(st.st_mode) ? "dir" :
                              S_ISREG(st.st_mode) ? "file" : "other");
        if (S_ISREG(st.st_mode)) jb_kv_int(&b, "size", (long long)st.st_size);
        fsapi_fmt_rfc3339((long long)st.st_mtime, mt, sizeof mt);
        jb_kv_str(&b, "mtime", mt);
        fsapi_fmt_etag((long long)st.st_size, (long long)st.st_mtime, et, sizeof et);
        jb_kv_str(&b, "etag", et);
        jb_obj_close(&b);
    }
    jb_arr_close(&b);
    jb_obj_close(&b);
    closedir(d);

    if (b.err) { jb_free(&b); errno = ENOMEM; return -1; }
    s = jb_take(&b, NULL);
    if (!s) { errno = ENOMEM; return -1; }
    *out_json = s;
    return 0;
}

/* ====================================================================
 * Formatters
 * ==================================================================== */

void fsapi_fmt_etag(long long size, long long mtime, char *out, size_t outn)
{
    snprintf(out, outn, "%lld-%lld", size, mtime);
}

void fsapi_fmt_rfc3339(long long secs, char *out, size_t outn)
{
    time_t t = (time_t)secs;
    struct tm tm;
    if (!gmtime_r(&t, &tm)) { if (outn) out[0] = '\0'; return; }
    strftime(out, outn, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void fsapi_fmt_httpdate(long long secs, char *out, size_t outn)
{
    time_t t = (time_t)secs;
    struct tm tm;
    if (!gmtime_r(&t, &tm)) { if (outn) out[0] = '\0'; return; }
    /* RFC 7231 IMF-fixdate; the C locale yields the required English. */
    strftime(out, outn, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

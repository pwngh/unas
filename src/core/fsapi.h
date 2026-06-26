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

/* src/core/fsapi.h — the jailed filesystem behind the API.
 *
 * Every operation that touches the share lives here, so the durability
 * policy (write to a temp file in the same directory, fsync, publish by
 * rename or link, fsync the parent) and the path jail (percent-decode,
 * then reject any "..", NUL, or control byte before a single syscall runs)
 * sit in one place. The HTTP layer never builds a filesystem path itself; it calls
 * fsapi_resolve and passes the result here.
 *
 * All operations return 0 on success or -1 with errno set, so the caller
 * maps errno -> HTTP status uniformly. Strict C99 + POSIX.1-2008.
 */
#ifndef UNAS_FSAPI_H
#define UNAS_FSAPI_H

#include <stddef.h>
#include <stdbool.h>

/* Turn a URL subpath (whatever follows "/v1/fs") into a full filesystem
 * path inside `root`. First it undoes percent-encoding (URLs spell a space
 * as "%20", a slash as "%2F", and so on). Then it walks the path purely by
 * reading the text -- no disk touched yet -- and refuses anything that
 * could climb out: a lone "." (meaning "this folder") is harmlessly
 * skipped, but a ".." ("go up one"), a NUL byte, or any control byte fails
 * with EINVAL. A trailing slash means "this is a directory": *is_dir is set
 * true and the slash is trimmed from `out`. The empty path and "/" both
 * point at `root` itself, again with *is_dir true. Returns 0, or -1 with
 * errno = EINVAL (bad path) or ENAMETOOLONG (result wouldn't fit). */
int fsapi_resolve(const char *root, const char *urlsub,
                  char *out, size_t outn, bool *is_dir);

/* Second line of defense, this time against the real disk. The text-only
 * check in fsapi_resolve blocks "..", but it can't see a symlink (a file
 * that's really a pointer to somewhere else) that already lives in the
 * share and aims outside it. So take `abspath`, find the longest leading
 * part that actually exists on disk, and ask the OS to resolve it fully --
 * following every symlink -- into one true "canonical" path. (A tail that
 * hasn't been created yet can't hide a symlink, so checking the existing
 * prefix is enough.) That canonical path must sit under the canonicalized
 * `root`. Returns 0 if it stays inside, -1 with errno = EACCES if it
 * escapes. Call after fsapi_resolve, before the operation runs. */
int fsapi_contained(const char *root, const char *abspath);

/* Startup validation: root exists, is a directory, and we are not euid 0
 * (the UNAS export squashes root). 0, or -1 with a message in `err`. */
int fsapi_check_root(const char *root, char *err, size_t errn);

/* Write the body to disk so it survives a crash. It first uses up the
 * bytes the HTTP layer already had in hand (`prebuf[0..prelen)`), then
 * pulls the rest from `src_fd` until exactly `content_length` bytes are
 * saved. The durable recipe is always the same four steps: write into a
 * temp file in the same directory; fsync it (force the bytes out of memory
 * onto the physical disk); publish it under the real name; then fsync the
 * directory (so the new name itself is on disk). Missing parent
 * directories are created along the way. The publish step depends on
 * `excl`: when true it uses link(2), which refuses with EEXIST -- in one
 * indivisible step -- if the target already exists (create-only writes,
 * i.e. the If-None-Match: * request); when false it uses rename(2), which
 * overwrites any existing file. Returns 0, or -1 with errno set. */
int fsapi_write_stream(const char *abspath, int src_fd,
                       const char *prebuf, size_t prelen,
                       long long content_length, bool excl);

/* mkdir -p `abspath`. 0 / -1 (errno). */
int fsapi_mkdirs(const char *abspath);

/* rename(2) src -> dst, creating dst's parent and fsync'ing both dirs. */
int fsapi_move(const char *src, const char *dst);

/* Durable copy src -> dst (temp -> fsync -> rename). 0 / -1 (errno). */
int fsapi_copy(const char *src, const char *dst);

/* Remove a file, or a directory (recursively when `recursive`). 0 / -1. */
int fsapi_rmtree(const char *abspath, bool recursive);

/* readdir(abspath) -> a freshly malloc'd JSON listing (caller frees),
 * with `urlpath` echoed as the "path" field. 0 / -1 (errno). */
int fsapi_list_json(const char *abspath, const char *urlpath, char **out_json);

/* Formatters shared with the HTTP layer (ETag, Last-Modified, listing
 * mtimes). `size`/`secs` come straight from struct stat fields. */
void fsapi_fmt_etag(long long size, long long mtime, char *out, size_t outn);
void fsapi_fmt_rfc3339(long long secs, char *out, size_t outn);
void fsapi_fmt_httpdate(long long secs, char *out, size_t outn);

#endif /* UNAS_FSAPI_H */

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

/* Resolve a URL subpath (the part after "/v1/fs") to an absolute path
 * under `root`. Percent-decodes first, then lexically rejects traversal:
 * any "." is skipped, any ".." / NUL / control byte fails with EINVAL.
 * A trailing slash sets *is_dir true (and is stripped from `out`); the
 * empty path and "/" resolve to the root itself with *is_dir true.
 * Returns 0, or -1 with errno = EINVAL (bad path) or ENAMETOOLONG. */
int fsapi_resolve(const char *root, const char *urlsub,
                  char *out, size_t outn, bool *is_dir);

/* Filesystem-level containment: the lexical jail in fsapi_resolve stops
 * "..", but not a symlink already in the share that points outside it.
 * Canonicalize the longest existing prefix of `abspath` (a not-yet-created
 * tail carries no symlink of its own) and require it to sit under the
 * canonicalized `root`. 0 if contained, -1 with errno = EACCES if it
 * escapes. Call after fsapi_resolve, before the operation runs. */
int fsapi_contained(const char *root, const char *abspath);

/* Startup validation: root exists, is a directory, and we are not euid 0
 * (the UNAS export squashes root). 0, or -1 with a message in `err`. */
int fsapi_check_root(const char *root, char *err, size_t errn);

/* Durable streaming write. Consumes `prebuf[0..prelen)` (body bytes the
 * HTTP layer already read) then reads the remainder from `src_fd` until
 * `content_length` bytes are stored, via temp -> fsync -> publish ->
 * fsync(dir). Parent directories are created. When `excl` is true the
 * publish uses link(2), so an existing target fails with EEXIST atomically
 * (create-only / If-None-Match: *); otherwise rename(2) (clobber).
 * 0 / -1 (errno). */
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

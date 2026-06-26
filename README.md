# unas

**unas turns a NAS share into a plain HTTP file API.** No data model, no
database, no vendor SDK. The URL path *is* the path on disk, the HTTP
method *is* the filesystem call, and a read returns the file's bytes
byte-for-byte.

A small always-on POSIX box mounts the share (NFS/SMB); `unasd` serves it
over HTTP/1.1 so anything that speaks HTTP — `curl`, a Python script, a
browser, a menu-bar app — can read, write, list, move, and delete files
on hardware you own, with no NFS client on the caller's side.

> **New — works with the UniFi ENAS too.** Ubiquiti's Enterprise NAS
> (shipped June 2026 — 16-bay ZFS, iSCSI, dual 25GbE) is still a locked
> UniFi OS appliance: files leave it only over SMB/NFS/iSCSI and the UniFi
> Drive apps, with no inbound HTTP file API. Same gap as the UNAS, same
> fix — mount an ENAS share on the host and `unasd` re-exposes it unchanged.

## The whole idea, in one table

Every request is a shell command you already know. The method picks the
operation; the path after `/v1/fs` is the path under the share root.

| You'd type at a shell | unas request |
|---|---|
| `cat Photos/notes.txt`            | `GET    /v1/fs/Photos/notes.txt` |
| `ls Photos/`                      | `GET    /v1/fs/Photos/`  *(trailing slash → JSON listing)* |
| `cp local.txt Photos/notes.txt`   | `PUT    /v1/fs/Photos/notes.txt`  *(body = bytes)* |
| `rm Photos/notes.txt`             | `DELETE /v1/fs/Photos/notes.txt` |
| `mv Photos/a.jpg Archive/a.jpg`   | `MOVE   /v1/fs/Photos/a.jpg`  *(`Destination:` = target)* |
| `cp Photos/a.jpg Archive/a.jpg`   | `COPY   /v1/fs/Photos/a.jpg`  *(`Destination:` = target)* |
| `mkdir -p Archive/2026`           | `PUT    /v1/fs/Archive/2026/` *(trailing slash → directory)* |

That's the entire surface. The sections below add only what HTTP gives
you for free: conditional writes, byte ranges, and a stable error code on
every failure.

## See it on the wire

Every route but `/healthz` needs a bearer token; `unasd` prints one at
startup (see [Build & run](#build--run)). The transcripts below assume
`A="Authorization: Bearer $TOK"`. Responses are real, trimmed to the
headers that carry meaning.

A listing is just the directory, as JSON. `type` is `dir`/`file`/`other`;
`size` is present only on regular files:

```console
$ curl -H "$A" localhost:8088/v1/fs/Photos/
{"path":"/Photos/","entries":[
  {"name":"notes.txt","type":"file","size":11,"mtime":"2026-06-25T16:49:51Z","etag":"11-1782406191"},
  {"name":"cat.jpg","type":"file","size":100000,"mtime":"2026-06-25T16:49:51Z","etag":"100000-1782406191"}]}
```
*(wire JSON is a single line; wrapped here to read.)*

A read is the file's bytes, with cache and range metadata attached:

```console
$ curl -i -H "$A" localhost:8088/v1/fs/Photos/notes.txt
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 11
ETag: "11-1782406191"
Last-Modified: Thu, 25 Jun 2026 16:49:51 GMT
Accept-Ranges: bytes

hello unas
```

Ask for five bytes, get five bytes — `206`, not `200`:

```console
$ curl -i -H "$A" -H 'Range: bytes=0-4' localhost:8088/v1/fs/Photos/notes.txt
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-4/11
Content-Length: 5

hello
```

A write is `PUT`; the body is the file. It returns `201 Created` (or
`200` on overwrite) and the `Location` it landed at. Use `curl -T` — that
sends a `PUT`; a `POST` (e.g. `curl --data-binary @f`) is rejected `405`:

```console
$ curl -i -T cat.jpg -H "$A" localhost:8088/v1/fs/Photos/cat.jpg
HTTP/1.1 201 Created
Location: /v1/fs/Photos/cat.jpg
```

Every failure is the same envelope — a stable `code` to branch on, the
HTTP status, the path you asked for, and a human `message`:

```console
$ curl -i -H "$A" localhost:8088/v1/fs/Photos/missing.txt
HTTP/1.1 404 Not Found

{"error":{"code":"ENOENT","http":404,"path":"/Photos/missing.txt",
          "message":"No such file or directory"}}
```

## More verbs

Rename, copy, and delete follow the same shape. `MOVE`/`COPY` take the
target in a `Destination: /v1/fs/...` header and return `201 Created`:

```console
$ curl -i -X MOVE -H "$A" -H 'Destination: /v1/fs/Archive/cat.jpg' \
       localhost:8088/v1/fs/Photos/cat.jpg
HTTP/1.1 201 Created
Location: /v1/fs/Archive/cat.jpg
```

`DELETE` returns `204 No Content` with an empty body. A trailing slash
plus `Depth: infinity` removes a directory tree:

```console
$ curl -i -X DELETE -H "$A" localhost:8088/v1/fs/Archive/cat.jpg
HTTP/1.1 204 No Content
```

`MOVE` is rename-only and same-filesystem only; across devices, `COPY`
then `DELETE`.

## Conditional writes

The two conditionals differ in strength, and the difference is the point.

`If-None-Match: *` is an **atomic** create-only. The publish is a
`link(2)`, so of two racing creators exactly one wins `201`; the loser
gets `412` — settled in the kernel, with no window between:

```console
$ curl -i -T new.txt -H "$A" -H 'If-None-Match: *' localhost:8088/v1/fs/Photos/once.txt
HTTP/1.1 201 Created                       # first writer wins

$ curl -i -T new.txt -H "$A" -H 'If-None-Match: *' localhost:8088/v1/fs/Photos/once.txt
HTTP/1.1 412 Precondition Failed           # loser
{"error":{"code":"EEXIST","http":412,"path":"/Photos/once.txt",
          "message":"exists (If-None-Match: *)"}}
```

`If-Match: "<etag>"` is a compare-and-swap — **advisory**, checked just
before the write. It catches a stale client, not a concurrent writer:

```console
$ curl -i -T new.txt -H "$A" -H 'If-Match: "999-999"' localhost:8088/v1/fs/Photos/notes.txt
HTTP/1.1 412 Precondition Failed
{"error":{"code":"EMISMATCH","http":412,"path":"/Photos/notes.txt",
          "message":"ETag does not match (If-Match)"}}
```

**ETag** is the wire tag `"<size>-<mtime_epoch>"` — e.g. `"11-1782406191"`,
a fingerprint of size and mtime, no hash, no read of the bytes.

- Reads and listings return it; the conditional headers compare against it.
- It changes on any ordinary write (size or mtime moves).
- Blind spot: a same-second, same-size overwrite reuses the tag (mtime is
  whole-second). Not a content hash — don't treat it as one.

## Error model

```
ENOENT                 -> 404      EINVAL (bad path / request)  -> 400
EACCES / EPERM / EROFS -> 403      bad/missing token            -> 401
EEXIST                 -> 409      unknown method               -> 405
EISDIR / ENOTDIR       -> 409      If-Match / If-None-Match fail -> 412
ENOTEMPTY              -> 409      no Content-Length on PUT      -> 411
ENOSPC / EDQUOT        -> 507      Range unsatisfiable          -> 416
ENAMETOOLONG           -> 414      too many connections (cap)   -> 503
```

Branch on the `code`, never parse the `message`. A filesystem failure
carries its C `errno` name (`ENOENT`, `EEXIST`, …); an HTTP-layer failure
carries a named protocol code — `EAUTH` 401, `EMETHOD` 405, `ELENGTH` 411,
`EMISMATCH` 412, `ERANGE` 416, `EBUSY` 503. Anything off the table is
`500`/`EIO`. Auth failures omit `path` so a bad token reveals nothing
about the target.

Two consequences worth knowing: PUT bodies must be length-delimited — no
`Content-Length` is `411`, and `Transfer-Encoding: chunked` is unsupported.
And at `UNAS_MAX_CONN` the server answers `503`/`EBUSY` with `Retry-After`
instead of forking.

## Headers (small, all optional except auth)

| Header | On | Effect |
|---|---|---|
| `Authorization: Bearer <tok>` | all but `/healthz` | constant-time compare |
| `If-None-Match: *` | PUT | atomic create-only -> 412 if exists |
| `If-Match: "<etag>"` | PUT/DELETE/MOVE/COPY | advisory CAS -> 412 on mismatch |
| `Range: bytes=a-b` | GET | `206` + `Content-Range` (or `416`) |
| `Destination: /v1/fs/...` | MOVE/COPY | the rename/copy target |
| `Depth: infinity` | DELETE on a dir | recursive remove |
| `Expect: 100-continue` | PUT | server sends `100` before the body |

## Introspection

Three read-only endpoints. `/healthz` is the one route served before the
auth gate — a liveness probe must answer without the token:

```console
$ curl localhost:8088/healthz
ok
```

`/v1/status` reports the live mount state. `mounted` is true when the root
stats as a directory; `writable` adds an `access(W_OK)` check:

```console
$ curl -H "$A" localhost:8088/v1/status
{"version":"unas/1.0","addr":"127.0.0.1","port":8088,"root":"/tmp/unas_share",
 "mounted":true,"writable":true,"uptime_s":0}
```

`/v1/shares` reports capacity straight off `statvfs`. A UNAS export
measures the whole storage pool, not this share's quota, so `scope` is
`"pool"` (if `statvfs` fails, `scope` is `"unknown"`, `total_bytes` is
`null`, and the other byte counts are omitted):

```console
$ curl -H "$A" localhost:8088/v1/shares
{"shares":[{"name":"unas_share","path":"/tmp/unas_share",
  "total_bytes":994662584320,"free_bytes":762697428992,
  "avail_bytes":762697428992,"scope":"pool"}]}
```

## Desktop companion (menu bar / tray)

There's an optional [companion app](companion/) for macOS, Windows, and
Linux. On first run it walks you through setup — mount the share (your OS
handles the login), then pick the folder to serve — and from then on it
launches and supervises `unasd` for you and shows live status: serving
state, the base URL, the bearer token (reveal + copy), and free space, with
one click to open the share or copy a ready-to-run `curl`. It adds nothing
to the daemon — it drives `unasd` and reads its HTTP API and `UNAS_STATE`
files, the same surfaces any client uses.

Built with [Tauri](https://tauri.app) — one small native binary per OS on
the system webview, no bundled browser. See [companion/](companion/) to
build and run it, or open `companion/ui/index.html` in a browser to preview
the UI.

## Why it has to run on a host (not the NAS itself)

UniFi OS is locked. The UNAS — and the new Enterprise NAS (ENAS) — expose
only SMB/NFS shares (plus iSCSI and the UniFi Drive apps), with no native
inbound file API. So `unasd` runs on any small always-on host that mounts
the share and re-exposes it over HTTP.

## Path safety

A request path is confined to the share root: `..`, percent-encoded
traversal, NUL, and control bytes are rejected before any syscall, and a
symlink inside the share that resolves outside the root is refused `403`.
`unasd` also refuses to start as `root` or to serve `/`.

## Build & run

```sh
cd unas
./configure        # probes the toolchain, writes config.mk
make               # builds libunas.a + unasd
make test          # unit tests + a curl-driven end-to-end run

# serve a mounted share on loopback
./unasd --token "$(openssl rand -hex 16)" /Users/you/mnt/unas/share
#  -> PORT=8088   (PORT and TOKEN are printed on stdout)
```

Options (flags or environment):

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--addr H` | `UNAS_ADDR` | `127.0.0.1` | bind address (`0.0.0.0` for LAN) |
| `--port N` | `UNAS_PORT` | `8088` | TCP port (`0` = ephemeral, printed) |
| `--token T` | `UNAS_TOKEN` | *generated* | bearer token |
| `--token-file F` | `UNAS_TOKEN_FILE` | — | read the token from a file |
| (positional) | `UNAS_ROOT` | — | the mounted share root (required) |
| — | `UNAS_IO_TIMEOUT` | `30` | per-connection idle timeout (s); `0` disables |
| — | `UNAS_MAX_CONN` | `256` | max concurrent connections; excess get `503`; `0` = unlimited |

If no token is supplied one is generated and printed. The daemon always
announces `PORT=<n>` and `TOKEN=<tok>` on stdout; additionally, with
`UNAS_STATE` set, it writes the files `unas.port` and `unas.token` (mode
0600) into that directory for a UI to read.

## Performance

Loopback, Apple M1 Max, pagecache-warm — figures that isolate unasd's overhead,
**not** production rates. In the field the NFS/SMB mount is the ceiling.

**Files** stream through one 64 KiB buffer — ~1 MB RAM per connection, any size:

| op (100 MB) | rate |
|---|---|
| GET | ~2.7 GB/s |
| PUT | ~2.7 GB/s (incl. `fsync`) |

```
1 GbE mount   █                       0.12 GB/s
10 GbE mount  █████████               1.2  GB/s
unasd         ██████████████████████  2.7  GB/s   ← never the bottleneck
```

**Requests** cost one `fork()` + one connection each (`Connection: close`):

| concurrency | req/s | p99 |
|---|---|---|
| 1  | ~660   | 1 ms |
| 8  | ~6,300 | 2 ms |
| 64 | ~5,700 | 102 ms |

A few thousand req/s — plenty for a personal/LAN API, not a high-RPS service (a
non-goal). `UNAS_MAX_CONN` (default 256) caps children; excess → `503`. For
heavy or public use, front with a reverse proxy (TLS + connection pooling).

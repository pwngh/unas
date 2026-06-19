# unas

**unas exposes a UNAS file share to the network as a plain HTTP API.**

A small always-on POSIX box mounts the share (NFS/SMB); `unasd` serves
it over HTTP/1.1 so any program — `curl`, a Python script, a browser, a
menu-bar app — can upload, fetch, list, move, and stat files on hardware
you own, with no NFS client and no vendor software on the caller's side.

It serves plain files, with no data model, records, or database in the
way: the path in a URL is the path on disk, each HTTP method is the file
operation its name implies, and reading a file returns its bytes
unaltered.

---

## Why it has to run on a host (not the UNAS)

UniFi OS is locked.

The UNAS exposes only SMB/NFS shares plus the UniFi Drive app, and there
is no native inbound file API. To solve this, we can run `unasd` on a
separate host that mounts the share and re-exposes it.

---

### Error model

```
ENOENT                 -> 404      EINVAL (bad path/traversal)  -> 400
EACCES / EPERM / EROFS -> 403      bad/missing token            -> 401
EEXIST                 -> 409      unknown method               -> 405
EISDIR / ENOTDIR       -> 409      If-Match / If-None-Match fail -> 412
ENOTEMPTY              -> 409      no Content-Length on PUT      -> 411
ENOSPC / EDQUOT        -> 507      Range unsatisfiable          -> 416
ENAMETOOLONG           -> 414
```

Each error carries a stable `code` a client can branch on instead of
parsing the human-readable `message`. For a filesystem failure the `code`
is the C `errno` name (`ENOENT`, `EEXIST`, …); HTTP-layer failures use a
named protocol code (`EAUTH` 401, `EMETHOD` 405, `ELENGTH` 411,
`EMISMATCH` 412, `ERANGE` 416). The `message` is there for human
readability:

```json
{ "error": { "code": "ENOENT", "http": 404, "path": "/Photos/x.jpg",
             "message": "no such file or directory" } }
```

### Headers (small, all optional except auth)

| Header | On | Effect |
|---|---|---|
| `Authorization: Bearer <tok>` | all but `/healthz` | constant-time compare |
| `If-None-Match: *` | PUT | create-only (no clobber) -> 412 if exists |
| `If-Match: "<etag>"` | PUT/DELETE/MOVE | compare-and-swap -> 412 on mismatch |
| `Range: bytes=a-b` | GET | `206` + `Content-Range` (or `416`) |
| `Destination: /v1/fs/...` | MOVE/COPY | the rename/copy target |
| `Depth: infinity` | DELETE on a dir | recursive remove |
| `Expect: 100-continue` | big PUT | server sends `100` before the body |

**ETag** is a fingerprint of a file's exact contents, letting a client
tie a request to a specific version. unas derives it from the file's
size and modification time — cheap, since it reads neither the file nor
a hash — and it changes whenever the file does. Reads and listings
return it, and the conditional headers above compare against it so a
write or delete can be made contingent on nothing having changed in
between.

---

## Build & run

```sh
cd unas
./configure        # probes the toolchain, writes config.mk
make               # builds libunas.a + unasd
make test          # unit tests + a curl-driven end-to-end run

# serve a mounted share on loopback
./unasd --token "$(openssl rand -hex 16)" /Users/you/mnt/unas/share
#  -> PORT=8088   (printed; the menu-bar app reads PORT/TOKEN from the state dir)
```

Options (flags or environment):

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--addr H` | `UNAS_ADDR` | `127.0.0.1` | bind address (`0.0.0.0` for LAN) |
| `--port N` | `UNAS_PORT` | `8088` | TCP port (`0` = ephemeral, printed) |
| `--token T` | `UNAS_TOKEN` | *generated* | bearer token |
| `--token-file F` | `UNAS_TOKEN_FILE` | — | read the token from a file |
| (positional) | `UNAS_ROOT` | — | the mounted share root (required) |

If no token is supplied one is generated and printed; with `UNAS_STATE`
set, the chosen port and token are written there (0600) for a UI to read.

### Examples

```sh
A="Authorization: Bearer $TOK"
curl -H "$A" --data-binary @cat.jpg  localhost:8088/v1/fs/Photos/cat.jpg   # upload (atomic)
curl -H "$A"                          localhost:8088/v1/fs/Photos/cat.jpg -O # download
curl -H "$A"                          localhost:8088/v1/fs/Photos/           # list -> JSON
curl -H "$A" -X MOVE -H 'Destination: /v1/fs/Archive/cat.jpg' \
                                      localhost:8088/v1/fs/Photos/cat.jpg     # rename
curl -H "$A"                          localhost:8088/v1/shares                # capacity + mount
```


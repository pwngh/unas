#!/bin/sh
# @pwngh/unas
#
# Copyright (c) Preston Neal
#
# This source code is licensed under the MIT license found in the
# LICENSE.md file in the root directory of this source tree.
#
# @license MIT

# tests/run.sh — end-to-end. POSIX sh.
#
# Starts unasd on a temp share root + ephemeral port, then drives the
# whole HTTP surface with curl and asserts on status codes, bodies, and
# byte-exact round-trips. Prints PASS/FAIL and exits non-zero on failure.

set -u
cd "$(dirname "$0")/.." || exit 2
REPO=$(pwd)

command -v curl >/dev/null 2>&1 || { echo "run.sh: curl not found" >&2; exit 2; }
[ -x ./unasd ] || { echo "run.sh: ./unasd not built" >&2; exit 2; }

PASS=0; FAIL=0
check_eq()  { if [ "$2" = "$3" ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: $1 (got '$2' want '$3')"; fi; }
check_has() { if printf '%s' "$2" | grep -q "$3"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: $1"; echo "  want substr: $3"; echo "  in: $2"; fi; }

ROOT=$(mktemp -d /tmp/unas_e2e_root.XXXXXX) || exit 2
TMP=$(mktemp -d /tmp/unas_e2e_tmp.XXXXXX)   || exit 2
LOG="$TMP/daemon.log"
TOKEN="testtoken123"

cleanup() { kill ${DPID:-} ${SPID:-} 2>/dev/null; rm -rf "$ROOT" "$TMP"; }
trap cleanup EXIT INT TERM

./unasd --addr 127.0.0.1 --port 0 --token "$TOKEN" "$ROOT" >"$LOG" 2>&1 &
DPID=$!

# learn the ephemeral port from the daemon's announcement
PORT=; i=0
while [ $i -lt 50 ]; do
    PORT=$(grep -m1 '^PORT=' "$LOG" 2>/dev/null | cut -d= -f2)
    [ -n "$PORT" ] && break
    sleep 0.1; i=$((i+1))
done
[ -n "$PORT" ] || { echo "run.sh: daemon never reported a port"; cat "$LOG"; exit 1; }

BASE="http://127.0.0.1:$PORT"
AUTH="Authorization: Bearer $TOKEN"

# wait until it answers
i=0
while [ $i -lt 50 ]; do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/healthz")" = "200" ] && break
    sleep 0.1; i=$((i+1))
done

# helper: status code of an authenticated request (extra args passed through)
code() { curl -s -o /dev/null -w '%{http_code}' -H "$AUTH" "$@"; }

# --- liveness + auth -------------------------------------------------
check_has "healthz body"   "$(curl -s "$BASE/healthz")"                  '^ok'
check_eq  "healthz code"   "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/healthz")" "200"
check_eq  "no-auth -> 401" "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/v1/status")" "401"
check_eq  "wrong-token -> 401"   "$(curl -s -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer wrongtoken' "$BASE/v1/status")" "401"
check_eq  "partial-token -> 401" "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer ${TOKEN%?}" "$BASE/v1/status")" "401"

# --- introspection ---------------------------------------------------
check_has "status version"  "$(curl -s -H "$AUTH" "$BASE/v1/status")" '"version":"unas/1.0"'
check_has "status writable" "$(curl -s -H "$AUTH" "$BASE/v1/status")" '"writable":true'
check_has "shares scope"    "$(curl -s -H "$AUTH" "$BASE/v1/shares")" '"scope":"pool"'

# --- PUT + GET round-trip (text) ------------------------------------
check_eq "put text 201" "$(code --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/notes/hello.txt")" "201"
curl -s -H "$AUTH" "$BASE/v1/fs/notes/hello.txt" -o "$TMP/hello.dl"
if cmp -s "$REPO/tests/fixtures/hello.txt" "$TMP/hello.dl"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: text round-trip differs"; fi

# HEAD + ETag header present
check_has "head etag" "$(curl -s -I -H "$AUTH" "$BASE/v1/fs/notes/hello.txt")" 'ETag:'
# ETag wire format is a quoted "<size>-<mtime>" — pin it so a format change is caught
check_has "etag wire format" "$(curl -s -I -H "$AUTH" "$BASE/v1/fs/notes/hello.txt")" 'ETag: "[0-9][0-9]*-[0-9][0-9]*"'

# --- listing ---------------------------------------------------------
check_has "list name" "$(curl -s -H "$AUTH" "$BASE/v1/fs/notes/")" '"name":"hello.txt"'
check_has "list type" "$(curl -s -H "$AUTH" "$BASE/v1/fs/notes/")" '"type":"file"'

# --- binary round-trip (byte-exact) ---------------------------------
code --data-binary @"$REPO/tests/fixtures/blob.bin" -X PUT "$BASE/v1/fs/data/blob.bin" >/dev/null
curl -s -H "$AUTH" "$BASE/v1/fs/data/blob.bin" -o "$TMP/blob.dl"
if cmp -s "$REPO/tests/fixtures/blob.bin" "$TMP/blob.dl"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: binary round-trip differs"; fi

# --- Range -----------------------------------------------------------
printf '0123456789' > "$TMP/digits.txt"
code --data-binary @"$TMP/digits.txt" -X PUT "$BASE/v1/fs/digits.txt" >/dev/null
check_eq "range body" "$(curl -s -H "$AUTH" -H 'Range: bytes=0-3' "$BASE/v1/fs/digits.txt")" "0123"
check_eq "range code" "$(code -H 'Range: bytes=0-3' "$BASE/v1/fs/digits.txt")" "206"

# --- mkdir -----------------------------------------------------------
check_eq  "mkdir 201"  "$(code -X PUT "$BASE/v1/fs/album/")" "201"
check_has "root lists album" "$(curl -s -H "$AUTH" "$BASE/v1/fs/")" '"name":"album"'

# --- MOVE / COPY -----------------------------------------------------
check_eq "move 201"     "$(code -X MOVE -H 'Destination: /v1/fs/notes/hi.txt' "$BASE/v1/fs/notes/hello.txt")" "201"
check_eq "move src gone" "$(code "$BASE/v1/fs/notes/hello.txt")" "404"
check_eq "move dst ok"   "$(code "$BASE/v1/fs/notes/hi.txt")"    "200"
check_eq "copy 201"     "$(code -X COPY -H 'Destination: /v1/fs/notes/copy.txt' "$BASE/v1/fs/notes/hi.txt")" "201"
check_eq "copy dst ok"   "$(code "$BASE/v1/fs/notes/copy.txt")" "200"

# --- DELETE ----------------------------------------------------------
check_eq "delete file 204" "$(code -X DELETE "$BASE/v1/fs/notes/copy.txt")" "204"
check_eq "deleted -> 404"  "$(code "$BASE/v1/fs/notes/copy.txt")" "404"
# non-empty dir without Depth -> 409; with Depth: infinity -> 204
code --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/rmme/f.txt" >/dev/null
check_eq "rmdir non-empty 409" "$(code -X DELETE "$BASE/v1/fs/rmme/")" "409"
check_eq "rmdir recursive 204" "$(code -X DELETE -H 'Depth: infinity' "$BASE/v1/fs/rmme/")" "204"

# --- If-Match compare-and-swap (DELETE + MOVE) -----------------------
code --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/cas/a.txt" >/dev/null
ETAG=$(curl -s -I -H "$AUTH" "$BASE/v1/fs/cas/a.txt" | grep -i '^etag:' | sed -e 's/^[^:]*:[[:space:]]*//' | tr -d '\r')
check_eq "if-match wrong -> 412 (DELETE)" "$(code -X DELETE -H 'If-Match: "0-0"' "$BASE/v1/fs/cas/a.txt")" "412"
check_eq "if-match wrong -> 412 (MOVE)"   "$(code -X MOVE -H 'If-Match: "0-0"' -H 'Destination: /v1/fs/cas/b.txt' "$BASE/v1/fs/cas/a.txt")" "412"
check_eq "if-match right -> 201 (MOVE)"   "$(code -X MOVE -H "If-Match: $ETAG" -H 'Destination: /v1/fs/cas/b.txt' "$BASE/v1/fs/cas/a.txt")" "201"
check_eq "if-match right -> 204 (DELETE)" "$(code -X DELETE -H "If-Match: $ETAG" "$BASE/v1/fs/cas/b.txt")" "204"

# --- create-only (If-None-Match: *) is an atomic create -> 201 then 412 ---
check_eq "create-only first 201" "$(code -H 'If-None-Match: *' --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/co/new.txt")" "201"
check_eq "create-only again 412" "$(code -H 'If-None-Match: *' --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/co/new.txt")" "412"

# --- malformed Range -> 416 (not a 206 of byte 0) --------------------
check_eq "bad range -> 416" "$(code -H 'Range: bytes=abc-def' "$BASE/v1/fs/digits.txt")" "416"

# --- jail + routing --------------------------------------------------
check_eq "traversal blocked"     "$(code --path-as-is "$BASE/v1/fs/../../etc/passwd")" "400"
check_eq "encoded traversal"     "$(code "$BASE/v1/fs/%2e%2e/secret")" "400"
# symlink escape: an in-share symlink resolving outside root -> 403; one within root still serves
ln -s /etc/hosts "$ROOT/escape" 2>/dev/null
check_eq "symlink escape 403"    "$(code "$BASE/v1/fs/escape")" "403"
printf 'ok' > "$ROOT/real.txt"; ln -s real.txt "$ROOT/good" 2>/dev/null
check_eq "symlink within 200"    "$(code "$BASE/v1/fs/good")" "200"
# a sibling dir whose name shares the root's prefix is NOT inside it (the
# boundary-char check, distinct from the resolves-elsewhere case above)
SIB="${ROOT}_sib"; mkdir -p "$SIB"; printf 'leak' > "$SIB/secret"; ln -s "$SIB/secret" "$ROOT/sibling" 2>/dev/null
check_eq "sibling-prefix not inside 403" "$(code "$BASE/v1/fs/sibling")" "403"
rm -rf "$SIB"
check_eq "missing file 404"      "$(code "$BASE/v1/fs/nope.txt")" "404"
check_eq "unknown endpoint 404"  "$(code "$BASE/v1/bogus")" "404"
check_eq "bad method 405"        "$(code -X DELETE "$BASE/v1/status")" "405"
check_has "405 has Allow hdr"    "$(curl -s -D - -o /dev/null -H "$AUTH" -X DELETE "$BASE/v1/status")" 'Allow: GET, HEAD'

# --- error surface: over-long name -> 414, permission denied -> 403 ---
LONG=$(awk 'BEGIN{for(i=0;i<5000;i++)printf "a"}')
check_eq "over-long name 414"    "$(code "$BASE/v1/fs/$LONG")" "414"
printf 'secret' > "$ROOT/locked.txt"; chmod 000 "$ROOT/locked.txt"
check_eq "perm-denied 403"       "$(code "$BASE/v1/fs/locked.txt")" "403"
chmod 644 "$ROOT/locked.txt"

# --- atomic create-only under concurrency: exactly one of N racers wins ---
mkdir -p "$TMP/race"
RN=12
RPIDS=""
i=1; while [ $i -le $RN ]; do
    ( printf '%s\n' "$(code -H 'If-None-Match: *' --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/raced.txt")" > "$TMP/race/$i" ) &
    RPIDS="$RPIDS $!"        # wait only the racers, not the long-lived daemon
    i=$((i+1))
done
for p in $RPIDS; do wait "$p"; done
# exactly one creator, and no racer ever overwrote (no 200) — the atomicity
# invariant, robust to a transient dropped request under load.
check_eq "create-only race: one 201 winner" "$(grep -lx 201 "$TMP/race"/* 2>/dev/null | wc -l | tr -d ' ')" "1"
check_eq "create-only race: no overwrite"   "$(grep -lx 200 "$TMP/race"/* 2>/dev/null | wc -l | tr -d ' ')" "0"
curl -s -H "$AUTH" "$BASE/v1/fs/raced.txt" -o "$TMP/raced.dl"
if cmp -s "$REPO/tests/fixtures/hello.txt" "$TMP/raced.dl"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL: raced file not byte-intact"; fi

# --- Expect: 100-continue handshake on PUT ---------------------------
check_eq "expect-100-continue 201" "$(code -H 'Expect: 100-continue' --data-binary @"$REPO/tests/fixtures/hello.txt" -X PUT "$BASE/v1/fs/exp/e.txt")" "201"

# --- UNAS_STATE: port/token files written 0600 for a UI to read ------
SSHARE=$(mktemp -d "$TMP/sshare.XXXXXX"); SSTATE=$(mktemp -d "$TMP/sstate.XXXXXX")
UNAS_STATE="$SSTATE" ./unasd --addr 127.0.0.1 --port 0 --token statetok "$SSHARE" >"$TMP/state.log" 2>&1 &
SPID=$!
j=0; while [ $j -lt 50 ]; do [ -s "$SSTATE/unas.port" ] && [ -s "$SSTATE/unas.token" ] && grep -q '^PORT=' "$TMP/state.log" && break; sleep 0.1; j=$((j+1)); done
check_has "state token content"   "$(cat "$SSTATE/unas.token" 2>/dev/null)" '^statetok$'
check_eq  "state port matches"    "$(tr -d '[:space:]' < "$SSTATE/unas.port" 2>/dev/null)" "$(grep -m1 '^PORT=' "$TMP/state.log" | cut -d= -f2)"
check_eq  "state token mode 0600" "$(ls -l "$SSTATE/unas.token" 2>/dev/null | cut -c1-10)" "-rw-------"
kill "$SPID" 2>/dev/null

# --- short body (Content-Length > bytes sent) leaves no file (atomic) -
if command -v nc >/dev/null 2>&1; then
    printf 'PUT /v1/fs/short.txt HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 100\r\n\r\nhello' "$TOKEN" | nc -w 2 127.0.0.1 "$PORT" >/dev/null 2>&1
    check_eq "short-body PUT leaves no file" "$(code "$BASE/v1/fs/short.txt")" "404"
fi

# --- serving the filesystem root is refused at startup (footgun) -----
./unasd --addr 127.0.0.1 --port 0 --token x / >"$TMP/rootrefuse.log" 2>&1
check_eq  "root=/ refused (exit 1)"  "$?" "1"
check_has "root=/ refusal message"   "$(cat "$TMP/rootrefuse.log")" 'refusing to serve the filesystem root'

echo "PASS $PASS  FAIL $FAIL"
[ "$FAIL" -eq 0 ]

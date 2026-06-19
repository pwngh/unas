#!/bin/sh
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

cleanup() { [ -n "${DPID:-}" ] && kill "$DPID" 2>/dev/null; rm -rf "$ROOT" "$TMP"; }
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

# --- malformed Range -> 416 (not a 206 of byte 0) --------------------
check_eq "bad range -> 416" "$(code -H 'Range: bytes=abc-def' "$BASE/v1/fs/digits.txt")" "416"

# --- jail + routing --------------------------------------------------
check_eq "traversal blocked"     "$(code --path-as-is "$BASE/v1/fs/../../etc/passwd")" "400"
check_eq "encoded traversal"     "$(code "$BASE/v1/fs/%2e%2e/secret")" "400"
check_eq "missing file 404"      "$(code "$BASE/v1/fs/nope.txt")" "404"
check_eq "unknown endpoint 404"  "$(code "$BASE/v1/bogus")" "404"
check_eq "bad method 405"        "$(code -X DELETE "$BASE/v1/status")" "405"

echo "PASS $PASS  FAIL $FAIL"
[ "$FAIL" -eq 0 ]

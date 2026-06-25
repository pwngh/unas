#!/bin/sh
# @pwngh/unas
#
# Copyright (c) Preston Neal
#
# This source code is licensed under the MIT license found in the
# LICENSE.md file in the root directory of this source tree.
#
# @license MIT

# tests/check-docs.sh — drift guard. Every errno -> HTTP row in the ERRMAP
# table (src/core/unasd.c) must be documented with the same status in the
# README error model. Exits non-zero on any divergence, so a code change
# the docs don't mirror is caught at `make test` time rather than by a reader.
#
# This is the cheap enforcement of "one source of truth": the table can be
# hand-copied into the README, but the copies can no longer silently drift.
set -u
cd "$(dirname "$0")/.." || exit 2
SRC=src/core/unasd.c
DOC=README.md

tmp=$(mktemp /tmp/unas_drift.XXXXXX) || exit 2
trap 'rm -f "$tmp"' EXIT INT TERM

# Rows look like:  { ENOENT,       404, "ENOENT" },   (EDQUOT is #ifdef-guarded)
grep -E '^[[:space:]]*\{[[:space:]]*E[A-Z]+,[[:space:]]*[0-9]+,' "$SRC" \
    | sed -E 's/^[[:space:]]*\{[[:space:]]*(E[A-Z]+),[[:space:]]*([0-9]+).*/\1 \2/' > "$tmp"

[ -s "$tmp" ] || { echo "check-docs: no ERRMAP rows found in $SRC" >&2; exit 2; }

fails=0
while read -r sym num; do
    # The README error table lists each symbol on a line carrying its status
    # in arrow form, e.g. "EACCES / EPERM / EROFS -> 403". Match the arrow,
    # not bare co-occurrence — the JSON example below the table also mentions
    # "ENOENT" and 404 on one line and would otherwise mask real drift.
    if ! grep -w "$sym" "$DOC" | grep -qE -- "->[[:space:]]*${num}([^0-9]|$)"; then
        echo "DRIFT: $SRC maps $sym -> $num, not documented as such in $DOC"
        fails=1
    fi
done < "$tmp"

[ "$fails" -eq 0 ] || { echo "check-docs: errno<->README drift detected" >&2; exit 1; }
echo "check-docs: errno table matches README ($(wc -l < "$tmp" | tr -d ' ') rows)"

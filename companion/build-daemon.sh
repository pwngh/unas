#!/bin/sh
# Build the unas daemon and stage it as a Tauri sidecar, so the companion
# bundles its own unasd (one app, no separate install). Run this before
# `npm run build`. On macOS it produces a universal (arm64 + x86_64) binary;
# elsewhere it builds for the host. The companion lives inside the unas repo,
# so the daemon source is just one directory up.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
dest="$here/src-tauri/binaries"
host=$(rustc -vV | sed -n 's/host: //p')

case "$(uname -s)" in
  Darwin) archflags="-arch arm64 -arch x86_64"
          triples="aarch64-apple-darwin x86_64-apple-darwin" ;;
  *)      archflags=""
          triples="$host" ;;
esac

cd "$repo"
make distclean >/dev/null 2>&1 || true
CFLAGS="$archflags" ./configure >/dev/null
make >/dev/null

mkdir -p "$dest"
# Tauri names sidecars binaries/<name>-<target-triple>; a universal binary
# satisfies every macOS triple, so stage it under each.
for t in $triples; do
  cp unasd "$dest/unasd-$t"
done
[ "$(uname -s)" = Darwin ] && lipo -info "$dest/unasd-$host" || true
echo "staged sidecar -> $dest"

# Leave the repo tree as a clean host build, not cross-arch.
make distclean >/dev/null 2>&1 || true
./configure >/dev/null && make >/dev/null

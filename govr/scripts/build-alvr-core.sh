#!/usr/bin/env bash
# Build ALVR v20.14.1's client core for the Oculus Go (Android 7.1 / API 25, arm64).
# Output: build/alvr/libalvr_client_core.so (stripped) + src/client/jni/alvr_client_core.h
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/ref/alvr"; OUT="$ROOT/build/alvr"; TAG=v20.14.1
source "$ROOT/scripts/alvr-env.sh"
if [[ ! -d "$SRC/.git" ]]; then
  git clone -q --depth 1 --branch "$TAG" https://github.com/alvr-org/ALVR.git "$SRC"
  git -C "$SRC" submodule update --init --depth 1
fi
if git -C "$SRC" apply --check "$ROOT/patches/alvr-$TAG-go.patch" 2>/dev/null; then
  git -C "$SRC" apply "$ROOT/patches/alvr-$TAG-go.patch"
fi
(cd "$SRC" && cargo build -p alvr_client_core --lib --features go --target aarch64-linux-android --release)
command -v cbindgen >/dev/null || cargo install -q cbindgen
(cd "$SRC/alvr/client_core" && cbindgen --quiet --output "$ROOT/src/client/jni/alvr_client_core.h")
mkdir -p "$OUT"
llvm-strip --strip-unneeded -o "$OUT/libalvr_client_core.so" \
  "$SRC/target/aarch64-linux-android/release/libalvr_client_core.so"
ls -lh "$OUT/libalvr_client_core.so"

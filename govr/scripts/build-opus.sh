#!/usr/bin/env bash
# Cross-compile libopus (static) for the Go: build/opus/{include,lib/libopus.a}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; TC="$ROOT/toolchain"
SRC="$TC/opus-1.5.2"; OUT="$ROOT/build/opus"
[[ -f "$OUT/lib/libopus.a" ]] && { echo "already built: $OUT"; exit 0; }
"$TC/cmake/bin/cmake" -S "$SRC" -B "$OUT/build" -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$TC/android-ndk-r28c/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-25 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$OUT" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
  -DOPUS_BUILD_PROGRAMS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
make -C "$OUT/build" -j16 install >/dev/null
ls -l "$OUT/lib/libopus.a"

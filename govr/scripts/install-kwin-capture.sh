#!/usr/bin/env bash
# Build pc/kwin-capture and authorize it (and only it) for KWin's screencast protocol.
# Creates ~/.local/share/applications/govr-kwin-capture.desktop (user level, no root).
# Undo: rm ~/.local/share/applications/govr-kwin-capture.desktop && kbuildsycoca6
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/pc/kwin-capture"; OUT="$ROOT/build/kwin-capture"; mkdir -p "$OUT"
wayland-scanner client-header "$SRC/zkde-screencast-unstable-v1.xml" "$OUT/zkde-screencast-unstable-v1-client-protocol.h"
wayland-scanner private-code "$SRC/zkde-screencast-unstable-v1.xml" "$OUT/zkde-screencast-unstable-v1-protocol.c"
cc -O2 -Wall -o "$OUT/govr-kwin-capture" -I"$OUT" "$SRC/govr-kwin-capture.c" \
   "$OUT/zkde-screencast-unstable-v1-protocol.c" $(pkg-config --cflags --libs wayland-client)
BIN="$(realpath "$OUT/govr-kwin-capture")"
DESK="$HOME/.local/share/applications/govr-kwin-capture.desktop"
mkdir -p "$(dirname "$DESK")"
cat > "$DESK" <<DESKTOP
[Desktop Entry]
Type=Application
Name=GoVR KWin capture helper
Comment=Lets the GoVR headset streamer capture a screen without the portal dialog
Exec=$BIN
NoDisplay=true
X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
DESKTOP
kbuildsycoca6 >/dev/null 2>&1 || true
echo "built $BIN"; echo "authorized via $DESK"
"$BIN" --list

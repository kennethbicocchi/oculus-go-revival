#!/usr/bin/env bash
# install-desktop.sh: KDE integration for daily use (no root):
#  - "GoVR" control panel in the menu and on the desktop
#  - menu entries (GoVR Desktop / SteamVR / No Man's Sky / The Forest / Video VR / Stop / screen controls)
#  - global shortcuts Meta+Alt+D/Q/C/+/-/Space/,/./F/M (only where free)
#  - KWin screencast authorization for the capture helper (via install-kwin-capture.sh)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; A="$HOME/.local/share/applications"; mkdir -p "$A"
"$ROOT/scripts/install-kwin-capture.sh" >/dev/null
entry() {  # file name icon exec
  printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nIcon=%s\nTerminal=false\nCategories=Utility;\nKeywords=oculus;go;vr;govr;\n' \
    "$2" "$4" "$3" > "$A/$1.desktop"
}
entry govr-panel "GoVR" video-display "python3 $ROOT/pc/govr_panel.py"
entry govr-desktop "GoVR Desktop" video-display "$ROOT/scripts/govr-launch desktop"
entry govr-vr "GoVR SteamVR" applications-games "$ROOT/scripts/govr-launch vr"
entry govr-nms "GoVR No Man's Sky" applications-games "$ROOT/scripts/govr-launch nms"
entry govr-forest "GoVR The Forest" applications-games "$ROOT/scripts/govr-launch forest"
entry govr-youtube "GoVR Video VR" video-x-generic "$ROOT/scripts/govr-launch youtube"
entry govr-stop "GoVR Stop" process-stop "$ROOT/scripts/govr-launch stop"
for c in recenter:zoom-fit-best bigger:zoom-in smaller:zoom-out pause:media-playback-pause \
         back:media-seek-backward fwd:media-seek-forward layout:view-refresh menu:open-menu; do
  entry "govr-ctl-${c%%:*}" "GoVR ${c%%:*}" "${c##*:}" "$ROOT/pc/govr-ctl ${c%%:*}"
done
desk="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")"
[[ -d "$desk" ]] && cp "$A/govr-panel.desktop" "$desk/GoVR.desktop" && chmod +x "$desk/GoVR.desktop"
kbuildsycoca6 >/dev/null 2>&1 || true
python3 - <<'PY'
import dbus
ka = dbus.Interface(dbus.SessionBus().get_object("org.kde.kglobalaccel", "/kglobalaccel"),
                    "org.kde.KGlobalAccel")
M, A = 0x10000000, 0x08000000
keys = {"govr-desktop": 0x44, "govr-stop": 0x51, "govr-ctl-recenter": 0x43, "govr-ctl-bigger": 0x2b,
        "govr-ctl-smaller": 0x2d, "govr-ctl-pause": 0x20, "govr-ctl-back": 0x2c,
        "govr-ctl-fwd": 0x2e, "govr-ctl-layout": 0x46, "govr-ctl-menu": 0x4d}
for name, k in keys.items():
    comp, key = name + ".desktop", M | A | k
    if ka.isGlobalShortcutAvailable(key, comp):
        aid = [comp, "_launch", comp, "Launch"]; ka.doRegister(aid); ka.setShortcut(aid, [key], 0x4)
    else:
        print("shortcut already used, skipped:", name)
PY
echo "installed: open 'GoVR' from the menu or the desktop"

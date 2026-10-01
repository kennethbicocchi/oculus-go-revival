# Shared helpers, sourced by go-* scripts.
GO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHOTS="$GO_ROOT/shots"; LOGS="$GO_ROOT/logs"
mkdir -p "$SHOTS" "$LOGS"
ts() { date +%Y%m%d-%H%M%S; }
die() { echo "error: $*" >&2; exit 1; }
is_root() { [[ "$(adb shell id -u 2>/dev/null | tr -d '\r')" == 0 ]]; }
# Is scrcpy currently attached? (adb root would kill it)
scrcpy_running() { pgrep -x scrcpy >/dev/null; }

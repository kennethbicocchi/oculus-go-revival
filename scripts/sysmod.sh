#!/usr/bin/env bash
# sysmod.sh — mount / edit / flash helper for the Oculus Go system image.
#
# The whole point of this script is the `relabel` command: every write to a
# file inside the mounted image destroys its security.selinux xattr, and init
# then refuses to read the file with `avc: denied { read } ... unlabeled`.
# Forgetting that costs a full flash cycle. Here it is a single command.
#
# Usage:
#   ./sysmod.sh dump                 pull system_b off the device (~75 s)
#   ./sysmod.sh mount                mount the image read-write
#   ./sysmod.sh relabel <path>       restore the SELinux label on one file
#   ./sysmod.sh umount
#   ./sysmod.sh flash                flash it back (device must be in fastboot)
#   ./sysmod.sh backup               dump + keep a pristine .SAFE copy
#   ./sysmod.sh restore              flash the .SAFE copy back
#
# Paths given to `relabel` are relative to the mount point, e.g.
#   ./sysmod.sh relabel system/bin/init.oculus.properties.sh

set -euo pipefail

WORKDIR="${OCUGO_DIR:-$HOME/oculusgo}"
IMAGE="$WORKDIR/system.img"
SAFE="$WORKDIR/system.img.SAFE"
MNT="$WORKDIR/sysmount"

# Active slot is _b on the reference device. Override if yours differs:
#   OCUGO_SLOT=_a ./sysmod.sh flash
SLOT="${OCUGO_SLOT:-_b}"
BLOCK="/dev/block/sde21"          # system_b on MH-A32
LABEL="u:object_r:system_file:s0"

die() { echo "error: $*" >&2; exit 1; }

need() { command -v "$1" >/dev/null || die "$1 not found"; }

cmd_dump() {
  need adb
  mkdir -p "$WORKDIR"
  echo "Pulling $BLOCK -> $IMAGE (about 75 seconds)..."
  adb root >/dev/null 2>&1 || true
  sleep 2
  adb shell "dd if=$BLOCK" > "$IMAGE"
  ls -lh "$IMAGE"
}

cmd_backup() {
  [[ -f "$IMAGE" ]] || cmd_dump
  cp -n "$IMAGE" "$SAFE" && echo "Pristine copy kept at $SAFE" \
    || echo "$SAFE already exists, left untouched"

  # boot too, while we are at it
  if [[ ! -f "$WORKDIR/boot${SLOT}_original.img.SAFE" ]]; then
    echo "Backing up boot${SLOT}..."
    adb shell "dd if=/dev/block/bootdevice/by-name/boot${SLOT}" \
      > "$WORKDIR/boot${SLOT}_original.img.SAFE"
    ls -lh "$WORKDIR/boot${SLOT}_original.img.SAFE"
  fi
}

cmd_mount() {
  [[ -f "$IMAGE" ]] || die "no image at $IMAGE — run 'dump' first"
  mkdir -p "$MNT"
  mountpoint -q "$MNT" && die "already mounted at $MNT"
  sudo mount -o loop,rw "$IMAGE" "$MNT"
  echo "Mounted at $MNT"
  echo "Remember: run 'relabel <path>' after every edit."
}

cmd_umount() {
  mountpoint -q "$MNT" || die "not mounted"
  sudo umount "$MNT"
  echo "Unmounted."
}

cmd_relabel() {
  local target="${1:-}"
  [[ -n "$target" ]] || die "usage: $0 relabel <path-relative-to-mount>"
  mountpoint -q "$MNT" || die "image not mounted"
  local full="$MNT/$target"
  [[ -e "$full" ]] || die "no such file: $full"

  sudo setfattr -n security.selinux -v "${LABEL}\0" "$full"
  echo -n "now: "
  sudo getfattr --only-values -n security.selinux "$full" 2>/dev/null
  echo
}

cmd_flash() {
  need fastboot
  mountpoint -q "$MNT" && die "unmount the image before flashing"
  [[ -f "$IMAGE" ]] || die "no image at $IMAGE"
  fastboot devices | grep -q fastboot || die "device not in fastboot mode
  (power off, hold VOLUME DOWN, press power)"
  fastboot flash "system${SLOT}" "$IMAGE"
  fastboot reboot
}

cmd_restore() {
  need fastboot
  [[ -f "$SAFE" ]] || die "no pristine copy at $SAFE"
  fastboot devices | grep -q fastboot || die "device not in fastboot mode"
  echo "Flashing the pristine image back..."
  fastboot flash "system${SLOT}" "$SAFE"
  fastboot reboot
}

case "${1:-}" in
  dump)    cmd_dump ;;
  backup)  cmd_backup ;;
  mount)   cmd_mount ;;
  umount)  cmd_umount ;;
  relabel) shift; cmd_relabel "$@" ;;
  flash)   cmd_flash ;;
  restore) cmd_restore ;;
  *) sed -n '2,22p' "$0" | sed 's/^# \?//' ;;
esac

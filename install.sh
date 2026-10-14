#!/usr/bin/env bash
# RetroTV one-line installer for ReplayOS (Raspberry Pi 5). Run on the Pi as root:
#
#   curl -fsSL https://raw.githubusercontent.com/R3XCHRIS/retrotv-libretro/main/install.sh | bash
#
# Options (after "bash -s --"):
#   --uninstall          give the borrowed slot back and remove the core
#   --version vX.Y.Z     install a specific release instead of the latest
#   --storage DIR        ReplayOS storage root (default: the one ReplayOS uses)
#
# What it does: downloads the release (core + scripts), checks the SHA-256
# sums, copies the core to <storage>/retrotv/ and the scripts to
# <storage>/roms/_extra/, keeps an existing playlist.txt, then runs
# retrotv_install.sh, which points Extra > audio_video_test at RetroTV
# (with backups) and restarts ReplayOS.
set -euo pipefail

REPO="R3XCHRIS/retrotv-libretro"
BASE="${RETROTV_BASE:-https://github.com/$REPO/releases/latest/download}"
STORAGE=""
UNINSTALL=0

say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --uninstall) UNINSTALL=1 ;;
    --version) [[ $# -ge 2 ]] || die "--version needs a tag, e.g. v0.2.0"; BASE="https://github.com/$REPO/releases/download/$2"; shift ;;
    --storage) [[ $# -ge 2 ]] || die "--storage needs a directory"; STORAGE="$2"; shift ;;
    NULL) ;;                                           # ReplayOS passes NULL from the Extras menu
    *) die "unknown option: $1" ;;
  esac
  shift
done

[[ $EUID -eq 0 ]] || die "run as root (ReplayOS: ssh root@<pi>)"
[[ -f /opt/replay/cores/cores.cfg ]] || die "this doesn't look like ReplayOS (/opt/replay/cores/cores.cfg missing)"
[[ "$(uname -m)" == "aarch64" ]] || die "RetroTV is built for the Raspberry Pi 5 (aarch64), not $(uname -m)"

# The storage ReplayOS is using: system_storage in replay.cfg, else the first mounted one.
if [[ -z $STORAGE ]]; then
  kind=$(sed -n 's/^system_storage *= *"\{0,1\}\([a-z]*\)"\{0,1\}.*/\1/p' /media/sd/config/replay.cfg 2>/dev/null | head -1)
  [[ -n $kind && -d /media/$kind/roms ]] && STORAGE=/media/$kind
fi
if [[ -z $STORAGE ]]; then
  for r in /media/nfs /media/usb /media/nvme /media/sd; do [[ -d $r/roms ]] && { STORAGE=$r; break; }; done
fi
[[ -n $STORAGE && -d $STORAGE/roms ]] || die "no ReplayOS storage found (looked for <storage>/roms); use --storage DIR"
EXTRA=$STORAGE/roms/_extra
say "storage: $STORAGE"

if [[ $UNINSTALL -eq 1 ]]; then
  [[ -f $EXTRA/retrotv_uninstall.sh ]] || die "$EXTRA/retrotv_uninstall.sh not found - is RetroTV installed?"
  exec bash "$EXTRA/retrotv_uninstall.sh"
fi

command -v curl >/dev/null || die "curl is needed"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FILES="retrotv_libretro.so retrotv_install.sh retrotv_uninstall.sh retrotv_update.sh SHA256SUMS"
say "downloading from $BASE"
for f in $FILES; do
  curl -fsSL --connect-timeout 10 --retry 3 -o "$TMP/$f" "$BASE/$f" || die "download failed: $f (no internet, or no release yet?)"
done
( cd "$TMP" && sha256sum -c --quiet SHA256SUMS ) || die "checksum mismatch - download corrupted, not installing"
say "checksums OK"

mkdir -p "$STORAGE/retrotv/backup" "$EXTRA"
[[ -f $STORAGE/retrotv/retrotv_libretro.so ]] && cp "$STORAGE/retrotv/retrotv_libretro.so" "$STORAGE/retrotv/backup/retrotv_libretro.so.previous"
cp "$TMP/retrotv_libretro.so" "$STORAGE/retrotv/retrotv_libretro.so"   # plain cp: NFS squashes root
cp "$TMP/retrotv_install.sh" "$TMP/retrotv_uninstall.sh" "$TMP/retrotv_update.sh" "$EXTRA/"
say "core and scripts copied"

bash "$EXTRA/retrotv_install.sh"
say "next: put your playlist link in $STORAGE/retrotv/playlist.txt, then open Extra > audio_video_test"
say "update later from Extra > retrotv_update.sh, remove with Extra > retrotv_uninstall.sh"

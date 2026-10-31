#!/usr/bin/env bash
# RetroTV one-line installer for ReplayOS 2.1.0 or newer (Raspberry Pi 5). Run on the Pi as root:
#
#   curl -fsSL https://raw.githubusercontent.com/chris-skitch/retrotv-libretro/main/install.sh | bash
#
# Options (after "bash -s --"):
#   --uninstall          remove RetroTV (the core and its system)
#   --version vX.Y.Z     install a specific release instead of the latest
#   --storage DIR        ReplayOS storage root (default: the one ReplayOS uses)
#   --no-restart         don't restart ReplayOS (RetroTV's own updater uses this; exit 3 =
#                        a restart is still needed)
#
# What it does: downloads the release (core + scripts), checks the SHA-256
# sums, copies the core to <storage>/retrotv/ and the scripts to
# <storage>/roms/_extra/, keeps an existing playlist.txt, then runs
# retrotv_install.sh, which registers RetroTV as a system ("RetroTV" in the system
# list) and restarts ReplayOS.
set -euo pipefail

REPO="chris-skitch/retrotv-libretro"
BASE="${RETROTV_BASE:-https://github.com/$REPO/releases/latest/download}"
STORAGE=""
UNINSTALL=0
RESTARTARG=()

say() { echo "[retrotv] $*"; }
die() { echo "[retrotv] ERROR: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --uninstall) UNINSTALL=1 ;;
    --version) [[ $# -ge 2 ]] || die "--version needs a tag, e.g. v0.2.0"; BASE="https://github.com/$REPO/releases/download/$2"; shift ;;
    --storage) [[ $# -ge 2 ]] || die "--storage needs a directory"; STORAGE="$2"; shift ;;
    --slot) [[ $# -ge 2 ]] && shift; say "--slot is no longer needed: RetroTV is always its own system" ;;
    --no-restart) RESTARTARG=(--no-restart) ;;
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

v=$(dpkg-query -W -f='${Version}' replay-frontend 2>/dev/null || true)   # external systems arrived in 2.1.0
[[ -n $v && $(printf '%s\n' 2.1.0 "$v" | sort -V | head -1) == 2.1.0 ]] ||
  die "RetroTV needs ReplayOS 2.1.0 or newer (this is ${v:-unknown}): update ReplayOS first"
command -v curl >/dev/null || die "curl is needed"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FILES="retrotv_libretro.so retrotv_install.sh retrotv_uninstall.sh retrotv_update.sh SHA256SUMS"
EXTRAS="retrotv_launch.sh retrotv-zaparoo.toml"     # newer releases only: remote start, Zaparoo
say "downloading from $BASE"
for f in $FILES; do
  curl -fsSL --connect-timeout 10 --retry 3 -o "$TMP/$f" "$BASE/$f" || die "download failed: $f (no internet, or no release yet?)"
done
for f in $EXTRAS; do
  grep -q " $f\$" "$TMP/SHA256SUMS" && { curl -fsSL --connect-timeout 10 --retry 3 -o "$TMP/$f" "$BASE/$f" || die "download failed: $f"; }
done
( cd "$TMP" && sha256sum -c --quiet --ignore-missing SHA256SUMS ) || die "checksum mismatch - download corrupted, not installing"
say "checksums OK"

mkdir -p "$STORAGE/retrotv/backup" "$EXTRA"
[[ -f $STORAGE/retrotv/retrotv_libretro.so ]] && cp "$STORAGE/retrotv/retrotv_libretro.so" "$STORAGE/retrotv/backup/retrotv_libretro.so.previous"
cp "$TMP/retrotv_libretro.so" "$STORAGE/retrotv/retrotv_libretro.so"   # plain cp: NFS squashes root
for f in $EXTRAS; do [[ -f $TMP/$f ]] && cp "$TMP/$f" "$STORAGE/retrotv/$f"; done
cp "$TMP/retrotv_install.sh" "$TMP/retrotv_uninstall.sh" "$TMP/retrotv_update.sh" "$EXTRA/"
# ReplayOS won't run an Extras script that isn't executable ("Forbidden"); downloads aren't, and
# cp keeps an existing file's mode, so a script first copied without +x stayed that way
chmod 755 "$EXTRA/retrotv_install.sh" "$EXTRA/retrotv_uninstall.sh" "$EXTRA/retrotv_update.sh" || say "warning: could not mark the Extras scripts executable"
say "core and scripts copied"

rc=0
bash "$EXTRA/retrotv_install.sh" "${RESTARTARG[@]}" || rc=$?
if grep -qvE '^[[:space:]]*(#|$)' "$STORAGE/retrotv/playlist.txt" 2>/dev/null; then
  say "your playlist.txt is kept"
else
  say "next: put your playlist link in $STORAGE/retrotv/playlist.txt"
fi
say "update later from Extra > retrotv_update.sh, remove with Extra > retrotv_uninstall.sh"
exit $rc

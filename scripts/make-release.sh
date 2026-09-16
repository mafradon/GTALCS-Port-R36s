#!/bin/bash
# make-release.sh — assemble release/ (and gtalcs.zip) from the built binaries.
#
# release/ is exactly what goes into /roms/ports on the device, plus the three
# metadata files PortMaster wants alongside it.  Run `make portmaster installer`
# first; this script refuses to package a stale or missing binary.
set -eu
cd "$(dirname "$0")/.."

SRC=gta-lcs-portmaster
OUT=release

for f in gtalcs.armhf installer.armhf libclock_fix.so; do
    [ -f "$f" ] || { echo "missing build output: $f  (run: make portmaster installer)" >&2; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT"

# Freshly built binaries, not whatever happened to be sitting in the package dir.
cp gtalcs.armhf installer.armhf libclock_fix.so "$SRC/gtalcs/"
cp scripts/obb_dictionary.txt                   "$SRC/gtalcs/"

cp -a "$SRC/GTA Liberty City Stories.sh" "$OUT/"
cp -a "$SRC/gtalcs"                      "$OUT/"
cp -a "$SRC/port.json" "$SRC/gameinfo.xml" "$SRC/README.md" "$OUT/"

# A copy of the README travels INSIDE the port folder too: port.json ships only
# "GTA Liberty City Stories.sh" and "gtalcs", so a README left at the release
# root never reaches the device and the player has no controls reference there.
cp -a "$SRC/README.md" "$OUT/gtalcs/"

# conf/ is created on demand by the launcher; ship it empty so the folder exists.
mkdir -p "$OUT/gtalcs/conf"
rm -f "$OUT/gtalcs/conf/debug.env"          # never ship bring-up switches
rm -f "$OUT/gtalcs"/*.log "$OUT/gtalcs"/frame*.ppm 2>/dev/null || true

# PortMaster wants LF line endings and 644 on the launcher script.
chmod 644 "$OUT/GTA Liberty City Stories.sh"
chmod 755 "$OUT/gtalcs/gtalcs.armhf" "$OUT/gtalcs/installer.armhf"

( cd "$OUT" && zip -qr ../gtalcs.zip "GTA Liberty City Stories.sh" gtalcs )

echo "release/ contents:"
find "$OUT" -type f | sort | sed 's/^/  /'
echo
echo "release/ size: $(du -sh "$OUT" | cut -f1)   gtalcs.zip: $(du -h gtalcs.zip | cut -f1)"

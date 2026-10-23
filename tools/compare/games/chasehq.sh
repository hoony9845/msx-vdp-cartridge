#!/bin/sh
# Regression test with a real V9990 game: Chase H.Q. for MSX + V9990
# (DiHalt Studio, https://github.com/DiHaltStudio/chasehq-v9990).
# Downloads the release ROM (not stored in this repository), plays 90 s in
# openMSX with scripted input, records the V9990 port accesses and compares
# the screenshots with our core.
#
#   tools/compare/games/chasehq.sh [work_dir]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=${1:-$ROOT/build-compare/rec/chasehq}
ROM=$ROOT/build-compare/roms/chasehq_v9990.rom
URL=https://github.com/DiHaltStudio/chasehq-v9990/releases/download/v1.0/chasehq_v9990.rom
SHA1=1ed319a7df1341a0e9b068c7a6b59dd95f14f3ad

mkdir -p "$(dirname "$ROM")"
[ -f "$ROM" ] || curl -sSL -o "$ROM" "$URL"
echo "$SHA1  $ROM" | sha1sum -c --quiet

rm -rf "$WORK"
V9990_FRAMES=5400 V9990_SHOT_EVERY=60 SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} \
V9990_EXTRA_SCRIPT="$HERE/inputs/chasehq.tcl" \
    "$HERE/record.sh" "$WORK" -carta "$ROM" -romtype ASCII16
# frames before ~780 show the V9990 power-on state (C-BIOS boot + logo)
V9990_SKIP_BEFORE=780 "$HERE/compare_trace.sh" "$WORK"

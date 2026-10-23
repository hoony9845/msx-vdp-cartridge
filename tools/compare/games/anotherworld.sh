#!/bin/sh
# Regression test with Another World for MSX turbo R + V9990 + OPL4
# (Edd Biddulph, 2025; ASCII16 ROM, not stored here - pass its path).
# Runs on the C-BIOS based turbo R test machine with a MoonSound without wave
# ROM, records 4 minutes of the intro (no input needed) and compares.
#
#   tools/compare/games/anotherworld.sh "<path>/Another World - Edd Biddulph (2025).rom|.zip" [work_dir]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$1
WORK=${2:-$ROOT/build-compare/rec/anotherworld}
ROM=$ROOT/build-compare/roms/anotherworld.rom
mkdir -p "$(dirname "$ROM")"
case "$SRC" in
    *.zip) unzip -p "$SRC" '*.rom' > "$ROM" ;;
    *)     cp "$SRC" "$ROM" ;;
esac
"$HERE/setup_machines.sh" >/dev/null

rm -rf "$WORK"
MSX_MACHINE=C-BIOS_turboR_test V9990_FRAMES=${V9990_FRAMES:-14400} V9990_SHOT_EVERY=120 \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} \
    "$HERE/record.sh" "$WORK" -carta "$ROM" -romtype ASCII16 -ext moonsound_nowave
# frames before ~900: boot, detection screen and V9990 power-on state
V9990_SKIP_BEFORE=900 "$HERE/compare_trace.sh" "$WORK"

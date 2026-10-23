#!/bin/sh
# End-to-end test of record.sh + compare_trace.sh: a Z80 program drawing a
# P1 scene runs in openMSX, its port accesses are recorded, replayed through
# our core and compared with openMSX's screenshots.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$(cd "$HERE/../.." && pwd)/build-compare/selftest_record}
rm -rf "$OUT"; mkdir -p "$OUT"
python3 "$HERE/selftest_record.py" "$OUT/program.bin" "$OUT/loader.tcl"
V9990_FRAMES=400 V9990_SHOT_EVERY=100 \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} \
V9990_EXTRA_SCRIPT="$OUT/loader.tcl" \
    "$HERE/record.sh" "$OUT"
# The program starts at ~2 s (frame ~120) and its VRAM clear (LMMV) takes
# real time in openMSX but none in our core: judge only the shots taken after
# it has finished.
rm -f "$OUT/f000100.png" "$OUT/f000200.png"
"$HERE/compare_trace.sh" "$OUT"

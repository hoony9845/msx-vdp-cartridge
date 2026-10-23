#!/bin/sh
# Replay a recorded trace (record.sh) through our core and compare the
# screenshots with the ones openMSX took while recording.
#
#   compare_trace.sh <record_dir>
#
# Output: <record_dir>/report/report.md and side-by-side images.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
REC=$(realpath "$1")
WORK=$REC/compare
rm -rf "$WORK" "$REC/report"
mkdir -p "$WORK/traces" "$WORK/ours" "$WORK/openmsx"

cmake -S "$ROOT/host" -B "$ROOT/build-host" -G Ninja >/dev/null
ninja -C "$ROOT/build-host" replay >/dev/null

# geometry calibration shots (same openMSX settings as the recording)
python3 "$HERE/scenarios.py" "$WORK/traces" >/dev/null
for c in calibration calib_dots_256 calib_dots_512; do
    "$ROOT/build-host/replay" "$WORK/traces/$c.txt" "$WORK/ours" >/dev/null
    "$HERE/run_openmsx.sh" "$WORK/traces/$c.txt" "$WORK/openmsx" >/dev/null
done

python3 "$HERE/shot_window.py" "$REC/trace.txt" "$WORK/trace_window.txt"
"$ROOT/build-host/replay" "$WORK/trace_window.txt" "$WORK/ours" | tail -1
cp "$REC"/f*.png "$WORK/openmsx/"
# Shots before the software has programmed the V9990 show its power-on state,
# which is undefined on real hardware (openMSX: white, our core: black).
# Skip them with V9990_SKIP_BEFORE=<frame>.
if [ -n "$V9990_SKIP_BEFORE" ]; then
    for f in "$WORK"/openmsx/f*.png; do
        n=$(basename "$f" .png | sed 's/^f0*//')
        [ "${n:-0}" -lt "$V9990_SKIP_BEFORE" ] && rm -f "$f"
    done
fi
python3 "$HERE/compare.py" "$WORK/ours" "$WORK/openmsx" "$REC/report" --window

#!/bin/sh
# Replay a V9968 recording (record.sh) through our core and compare the
# screenshots with the ones openMSX took while recording.
#   compare_trace.sh <record_dir>
# Output: <record_dir>/report/report.md and side-by-side images.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
REC=$(realpath "$1")
WORK=$REC/compare
rm -rf "$WORK" "$REC/report"
mkdir -p "$WORK/traces" "$WORK/ours" "$WORK/openmsx"

cmake -S "$ROOT/host" -B "$ROOT/build-host" -G Ninja >/dev/null
ninja -C "$ROOT/build-host" replay9968 >/dev/null

# geometry calibration shots with the same openMSX settings
python3 "$HERE/scenarios.py" "$WORK/traces" calibration calib_dots_256 calib_dots_512 \
    calib_rows_256x212 >/dev/null
for t in "$WORK"/traces/*.txt; do
    "$ROOT/build-host/replay9968" "$t" "$WORK/ours" >/dev/null
    "$HERE/run_openmsx.sh" "$t" "$WORK/openmsx" >/dev/null
done

# each shot also rendered at the neighbouring frames (capture timing)
python3 "$HERE/../shot_window.py" "$REC/trace.txt" "$WORK/trace_window.txt"
"$ROOT/build-host/replay9968" "$WORK/trace_window.txt" "$WORK/ours" | tail -1
cp "$REC"/f*.png "$WORK/openmsx/"
# shots before the software has set the V9968 up show its power-on state
if [ -n "$V9968_SKIP_BEFORE" ]; then
    for f in "$WORK"/openmsx/f*.png; do
        n=$(basename "$f" .png | sed 's/^f0*//')
        [ "${n:-0}" -lt "$V9968_SKIP_BEFORE" ] && rm -f "$f"
    done
fi
python3 "$HERE/../compare.py" "$WORK/ours" "$WORK/openmsx" "$REC/report" --window

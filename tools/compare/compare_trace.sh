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

"$ROOT/build-host/replay" "$REC/trace.txt" "$WORK/ours"
cp "$REC"/f*.png "$WORK/openmsx/"
python3 "$HERE/compare.py" "$WORK/ours" "$WORK/openmsx" "$REC/report"

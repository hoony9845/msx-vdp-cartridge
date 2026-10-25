#!/bin/sh
# Generate the V9968 test scenarios, run them in our core and in the openMSX
# V9968 fork, compare.
#   tools/compare/v9968/run_compare.sh [work_dir] [scenario ...]
# Output: <work_dir>/report/report.md and side-by-side images
#         (ours | openMSX | differences in red).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WORK=${1:-$ROOT/build-compare9968}
[ $# -gt 0 ] && shift
rm -rf "$WORK/traces" "$WORK/ours" "$WORK/openmsx" "$WORK/report"
mkdir -p "$WORK/traces" "$WORK/ours" "$WORK/openmsx"

cmake -S "$ROOT/host" -B "$ROOT/build-host" -G Ninja >/dev/null
ninja -C "$ROOT/build-host" replay9968 >/dev/null

if [ $# -gt 0 ]; then
    python3 "$HERE/scenarios.py" "$WORK/traces" calibration calib_dots_256 calib_dots_512 \
        calib_rows_256x212 "$@" >/dev/null
else
    python3 "$HERE/scenarios.py" "$WORK/traces" >/dev/null
fi
for t in "$WORK"/traces/*.txt; do
    "$ROOT/build-host/replay9968" "$t" "$WORK/ours" >/dev/null
    if ! "$HERE/run_openmsx.sh" "$t" "$WORK/openmsx" > "$WORK/openmsx.log" 2>&1 ||
       grep -qE "ERROR|failed|Fatal" "$WORK/openmsx.log"; then
        cat "$WORK/openmsx.log"
        echo "openMSX run failed for $t" >&2
        exit 1
    fi
done
python3 "$HERE/../compare.py" "$WORK/ours" "$WORK/openmsx" "$WORK/report"

#!/bin/sh
# Generate the test scenarios, run them in our core and in openMSX, compare.
#   tools/compare/run_compare.sh [work_dir]
# Output: <work_dir>/report/report.md and side-by-side images
#         (ours | openMSX | differences in red).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=${1:-$ROOT/build-compare}
# start clean so a failed run can never compare stale screenshots
rm -rf "$WORK/traces" "$WORK/ours" "$WORK/openmsx" "$WORK/report"
mkdir -p "$WORK/traces" "$WORK/ours" "$WORK/openmsx"

cmake -S "$ROOT/host" -B "$ROOT/build-host" -G Ninja >/dev/null
ninja -C "$ROOT/build-host" replay >/dev/null

python3 "$HERE/scenarios.py" "$WORK/traces" >/dev/null
for t in "$WORK"/traces/*.txt; do
    "$ROOT/build-host/replay" "$t" "$WORK/ours" >/dev/null
    if ! "$HERE/run_openmsx.sh" "$t" "$WORK/openmsx" > "$WORK/openmsx.log" 2>&1 ||
       grep -qE "ERROR|failed|Fatal" "$WORK/openmsx.log"; then
        cat "$WORK/openmsx.log"
        echo "openMSX run failed for $t" >&2
        exit 1
    fi
done
python3 "$HERE/compare.py" "$WORK/ours" "$WORK/openmsx" "$WORK/report"

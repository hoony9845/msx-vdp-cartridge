#!/bin/sh
# Internal V9968 (98h-9Ch, geo3d 9Dh/9Fh) ROM -> external cartridge
# (88h-8Ch, 8Dh/8Fh) ROM, in one go:
#   1. trace the port accesses for 5 minutes (openMSX V9968 fork, MSX2+
#      with an internal V9968)
#   2. convert (v9968_port.py)
#   3. run both and check the cartridge picture (verify.sh); if it stays
#      black or still, convert again with the built-in VDP muted at start
#
#   convert.sh <internal.rom> <romtype> [output.rom]
#   romtype: ASCII8 for the V9968 sample demos, ASCII16 for the Geo3D ones
#   PORT_OPTS: more v9968_port.py options, e.g. "--take-likely" or "--force 1A2B"
HERE=$(cd "$(dirname "$0")" && pwd)
IN=$1; TYPE=$2
OUT=${3:-$(dirname "$IN")/$(basename "$IN" | sed -e 's/INTERNAL-98H\?/EXTERNAL-88H/I' -e 's/internal/external-88h/')}
[ "$OUT" != "$IN" ] || OUT="${IN%.*}-EXTERNAL-88H.ROM"
W=${WORK:-$(mktemp -d)}
mkdir -p "$W"
echo "tracing $IN (5 minutes of emulated time)..."
"$HERE/trace.sh" "$IN" "$W/trace.txt" "$TYPE" "${FRAMES:-18000}" || exit 1
[ -s "$W/trace.txt" ] || { echo "no trace"; exit 1; }
# traces of play sessions (play.sh) are used too
PLAYED=$(ls "${IN%.*}.traces/"*.txt 2>/dev/null)
[ -n "$PLAYED" ] && echo "with $(echo "$PLAYED" | wc -l) play-session trace(s) from ${IN%.*}.traces/"
python3 "$HERE/v9968_port.py" "$IN" "$W/trace.txt" $PLAYED -o "$OUT" -q $PORT_OPTS || echo "WARNING: see above"
echo "checking the picture of $OUT on the cartridge..."
if "$HERE/verify.sh" "$IN" "$OUT" "$TYPE" "$W/verify" "${VERIFY_FRAMES:-3600}" 300; then
    echo "-> $OUT"; exit 0
fi
echo "converting again with the built-in VDP muted at start..."
python3 "$HERE/v9968_port.py" "$IN" "$W/trace.txt" $PLAYED -o "$OUT" -q --mute-internal-vdp $PORT_OPTS || echo "WARNING: see above"
if "$HERE/verify.sh" "$IN" "$OUT" "$TYPE" "$W/verify_mute" "${VERIFY_FRAMES:-3600}" 300; then
    echo "-> $OUT (built-in VDP muted)"; exit 0
fi
echo "FAILED: $OUT does not run like the original"; exit 1

#!/bin/sh
# Run the internal ROM on an MSX2+ with an internal V9968 (98h) and the
# converted one on an MSX2+ with the V9968 cartridge (88h), screenshot both
# every N frames, and compare.
#   verify.sh <internal.rom> <external.rom> <romtype> <work_dir> [frames] [every]

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../compare/v9968/openmsx_env.sh"
I=$1; E=$2; T=$3; W=$4; F=${5:-7200}; N=${6:-300}
mkdir -p "$W"
run() {  # machine exts rom dir
    SHOT_DIR=$(realpath -m "$4") SHOT_EVERY=$N SHOT_FRAMES=$F HOME=$OPENMSX9968_HOME \
    SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 1500 \
        "$OPENMSX9968_BIN" -machine "$1" $2 -carta "$3" -romtype "$T" -script "$HERE/shots.tcl" < /dev/null \
        2>&1 | grep -v -i -e font -e VMware -e libEGL -e "Screen saved" || true
}
rm -rf "$W/internal" "$W/external"
run C-BIOS_MSX2+_V9968 "-ext geo3d" "$I" "$W/internal" &
run C-BIOS_MSX2+ "-ext HRA_V9968 -ext geo3d88" "$E" "$W/external" &
wait
python3 "$HERE/compare_shots.py" "$W/internal" "$W/external"

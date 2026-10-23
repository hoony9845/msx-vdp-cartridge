#!/bin/sh
# Record the V9990 port accesses of MSX software running in openMSX.
#
#   record.sh <out_dir> [openMSX arguments...]
#
# Examples:
#   record.sh rec/game  -diska game.dsk                       # play, quit openMSX to stop
#   V9990_FRAMES=1800 record.sh rec/demo -cart demo.rom      # stop after 30 s
#   MSX_MACHINE=Panasonic_FS-A1GT record.sh rec/x -diska x.dsk  # needs that system ROM
#
# The window is shown (unless SDL_VIDEODRIVER=dummy) so the software can be
# played. Afterwards compare it with our core:
#   compare_trace.sh <out_dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/openmsx_env.sh"
OUT=$1; shift
mkdir -p "$OUT"
V9990_RECORD=$(realpath "$OUT") HOME=$OPENMSX_HOME SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
    "$OPENMSX_BIN" -machine "${MSX_MACHINE:-C-BIOS_MSX2+}" -ext gfx9000 \
    -script "$HERE/record.tcl" "$@" 2>&1 | grep -v -i "font"

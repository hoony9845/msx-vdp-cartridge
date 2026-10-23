#!/bin/sh
# Run a V9990 trace in openMSX (C-BIOS MSX2+ with a GFX9000), headless.
#   run_openmsx.sh <trace.txt> <out_dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/openmsx_env.sh"
mkdir -p "$2"
V9990_TRACE=$(realpath "$1") V9990_OUT=$(realpath "$2") HOME=$OPENMSX_HOME \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=dummy \
    timeout ${TIMEOUT:-600} "$OPENMSX_BIN" -machine C-BIOS_MSX2+ -ext gfx9000 \
    -script "$HERE/run_openmsx.tcl" 2>&1 | grep -v -i "font"

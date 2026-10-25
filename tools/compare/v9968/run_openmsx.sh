#!/bin/sh
# Run a V9968 trace in the openMSX V9968 fork (C-BIOS MSX2+ with the V9968
# cartridge at 88h-8Ch and geo3d at 8Dh/8Fh), headless.
#   run_openmsx.sh <trace.txt> <out_dir>
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/openmsx_env.sh"
mkdir -p "$2"
V9968_TRACE=$(realpath "$1") V9968_OUT=$(realpath "$2") HOME=$OPENMSX9968_HOME \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIODRIVER=dummy \
    timeout ${TIMEOUT:-600} "$OPENMSX9968_BIN" -machine C-BIOS_MSX2+ -ext HRA_V9968 -ext geo3d88 \
    -script "$HERE/run_openmsx.tcl" 2>&1 | grep -v -i -e "font" -e "VMware" -e "libEGL"

#!/bin/sh
# Record the V9968 port accesses of MSX software running in the openMSX
# V9968 fork (C-BIOS MSX2+, Z80, V9968 cartridge at 88h-8Ch, geo3d 8Dh/8Fh).
#   record.sh <out_dir> [openMSX arguments...]
# e.g. V9968_FRAMES=1800 record.sh rec/demo -carta demo.rom -romtype ASCII8
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/openmsx_env.sh"
OUT=$1; shift
mkdir -p "$OUT"
V9968_RECORD=$(realpath "$OUT") HOME=$OPENMSX9968_HOME \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIODRIVER=dummy \
    "$OPENMSX9968_BIN" -machine "${MSX_MACHINE:-C-BIOS_MSX2+}" -ext HRA_V9968 -ext geo3d88 \
    -script "$HERE/record.tcl" "$@" 2>&1 | grep -v -i -e "font" -e "VMware" -e "libEGL"

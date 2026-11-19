#!/bin/sh
# Trace the internal-V9968 port accesses of a ROM in the openMSX V9968 fork.
#   trace.sh <rom> <out.txt> <romtype: ASCII8|ASCII16|...> [frames]
set -e
mkdir -p "$(dirname "$2")"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../compare/v9968/openmsx_env.sh"
PORT_TRACE_OUT=$(realpath -m "$2") PORT_TRACE_FRAMES=${4:-18000} HOME=$OPENMSX9968_HOME \
SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} \
    timeout ${PORT_TRACE_TIMEOUT:-1200} "$OPENMSX9968_BIN" -machine ${TRACE_MACHINE:-C-BIOS_MSX2+_V9968} ${TRACE_EXTS:--ext geo3d} \
    -carta "$1" $( [ "$3" = "-" ] || echo "-romtype $3" ) -script "$HERE/trace_io.tcl" < /dev/null 2>&1 | grep -v -i -e "font" -e "VMware" -e "libEGL" || true

#!/bin/sh
# Play an internal-V9968 game in an openMSX window while its port accesses
# are traced, for games that do not run through by themselves. Play as far
# as you can (every stage, menu, ending...), then close the window. Each
# session adds a trace next to the ROM (<rom>.traces/); convert.sh uses
# them all. Sessions can be repeated to cover more.
#
#   play.sh <internal.rom> <ASCII8|ASCII16|...>
#
# Keys are openMSX's: F10 console, F9 speed-up toggle, PAUSE pause.
HERE=$(cd "$(dirname "$0")" && pwd)
D="${1%.*}.traces"
mkdir -p "$D"
T="$D/play_$(date +%Y%m%d_%H%M%S).txt"
echo "playing $1; the trace goes to $T (close the window to finish)"
DISPLAY=${DISPLAY:-:0} SDL_VIDEODRIVER=x11 PORT_TRACE_TIMEOUT=0 SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-pulse} \
    "$HERE/trace.sh" "$1" "$T" "$2" 0
[ -s "$T" ] && echo "$(wc -l < "$T") sites in $T; now: tools/v9968_port/convert.sh \"$1\" $2"

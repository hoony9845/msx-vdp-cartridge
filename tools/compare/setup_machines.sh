#!/bin/sh
# Extra openMSX configurations for the comparison tools (no system ROMs needed):
#  - C-BIOS_turboR_test: C-BIOS patched to report an MSX turbo R, with R800
#    (make_cbios_turbor.py)
#  - moonsound_nowave: MoonSound (OPL4) with a blank wave ROM instead of the
#    Yamaha yrw801.rom - software detects the OPL4, sound is not needed here
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/openmsx_env.sh"
SHARE=$OPENMSX_HOME/.openMSX/share
python3 "$HERE/make_cbios_turbor.py" "$SHARE" "$OPENMSX_SYSTEM_DATA" >/dev/null
E=$SHARE/extensions/moonsound_nowave
mkdir -p "$E/roms"
[ -f "$E/roms/blank_wave.rom" ] || head -c 2097152 /dev/zero > "$E/roms/blank_wave.rom"
sed -e '/<sha1>32760893ce06dbe3930627755ba065cc3d8ec6ca<\/sha1>/d' \
    -e 's|<filename>yrw801.rom</filename>|<filename>roms/blank_wave.rom</filename>|' \
    "$OPENMSX_SYSTEM_DATA/extensions/moonsound.xml" > "$E/hardwareconfig.xml"
echo "openMSX test machines ready in $SHARE"

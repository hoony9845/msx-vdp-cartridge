# Environment for the V9968 + geo3d v2.0 openMSX fork built in
# ~/tools/openmsx-v9968 (alexmoncks/openMSX, branch geo3d-v2.0, on top of
# buppu3/openMSX's V9968 emulation). Source this file.
#
# One-time setup (no sudo needed):
#   git clone -b geo3d-v2.0 https://github.com/alexmoncks/openMSX ~/tools/openmsx-v9968/src
#   build it (see ~/tools/openmsx-v9968/build.sh: conda-forge toolchain)
# The C-BIOS machines and ROMs come from the openMSX 16.0 tree of
# ../openmsx_env.sh; the fork's own share/ (Tcl scripts, extensions) is used.

OPENMSX9968_ROOT=${OPENMSX9968_ROOT:-$HOME/tools/openmsx-v9968/src}
OPENMSX9968_BIN=$OPENMSX9968_ROOT/derived/x86_64-linux-opt/bin/openmsx
OPENMSX16_SHARE=${OPENMSX16_SHARE:-$HOME/tools/openmsx16/root/usr/share/openmsx}
export OPENMSX_SYSTEM_DATA=$OPENMSX9968_ROOT/share

OPENMSX9968_HOME=${OPENMSX9968_HOME:-$HOME/.cache/msx_v9968_openmsx}
U=$OPENMSX9968_HOME/.openMSX/share
mkdir -p "$U/machines"
[ -e "$U/systemroms" ] || ln -s "$OPENMSX16_SHARE/systemroms" "$U/systemroms"
for m in "$OPENMSX16_SHARE"/machines/C-BIOS_MSX2+*; do
    [ -e "$U/machines/$(basename "$m")" ] || ln -s "$m" "$U/machines/"
done
if [ ! -f "$U/settings.xml" ]; then
    FONT=$(fc-list :spacing=mono file 2>/dev/null | grep -i '\.ttf' | head -1 | sed 's/: *$//')
    [ -n "$FONT" ] || FONT=$(fc-list : file 2>/dev/null | grep -i 'mono.*\.ttf' | head -1 | sed 's/: *$//')
    cat > "$U/settings.xml" <<EOT
<!DOCTYPE settings SYSTEM 'settings.dtd'>
<settings>
  <settings>
    <setting id="renderer">SDLGL-PP</setting>
    <setting id="scale_factor">2</setting>
    <setting id="consolefont">$FONT</setting>
    <setting id="save_settings_on_exit">false</setting>
    <setting id="throttle">off</setting>
    <setting id="minframeskip">0</setting>
    <setting id="maxframeskip">0</setting>
    <setting id="scanline">0</setting>
    <setting id="blur">0</setting>
    <setting id="glow">0</setting>
    <setting id="noise">0</setting>
    <setting id="gamma">1.0</setting>
    <setting id="horizontal_stretch">320</setting>
  </settings>
</settings>
EOT
fi

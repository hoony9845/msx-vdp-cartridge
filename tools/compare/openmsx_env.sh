# Environment for an openMSX extracted (without root) into ~/tools/openmsx16.
# Source this file: . tools/compare/openmsx_env.sh
#
# openMSX 16.0 (Debian 11 build, runs on Ubuntu 20.04) is used because the
# V9990 P1/P2 rendering was fixed after 0.15 (openMSX commits of 2019-10 and
# 2020-05), which is what Ubuntu 20.04 ships.
#
# One-time setup (no sudo needed):
#   D=~/tools/openmsx16; mkdir -p $D/debs $D/root; cd $D/debs
#   for f in openmsx_16.0-1_amd64.deb openmsx-data_16.0-1_all.deb; do
#       curl -sSLO http://deb.debian.org/debian/pool/main/o/openmsx/$f; done
#   apt-get download cbios libsdl2-ttf-2.0-0
#   for f in *.deb; do dpkg -x $f $D/root; done
#   R=$D/root/usr/share
#   cp -r ~/tools/openmsx/root/usr/share/openmsx/machines/C-BIOS_* $R/openmsx/machines/  # from 0.15 data
#   mkdir -p $R/openmsx/systemroms; cp $R/cbios/*.rom $R/openmsx/systemroms/

OPENMSX_ROOT=${OPENMSX_ROOT:-$HOME/tools/openmsx16/root}
OPENMSX_BIN=$OPENMSX_ROOT/usr/bin/openmsx
export OPENMSX_SYSTEM_DATA=$OPENMSX_ROOT/usr/share/openmsx
export LD_LIBRARY_PATH=$OPENMSX_ROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# Private openMSX home with headless-friendly settings: SDL renderer, 640x480
# output with horizontal_stretch 320 (V9990 dots map to whole pixels, no
# interpolation), no throttling but no frame skipping either (screenshots must show the
# current frame), no scanline/blur/glow/noise/gamma effects, and a monospaced
# font that exists on this system.
OPENMSX_HOME=${OPENMSX_HOME:-$HOME/.cache/msx_v9990_openmsx16}
mkdir -p "$OPENMSX_HOME/.openMSX/share"
if [ ! -f "$OPENMSX_HOME/.openMSX/share/settings.xml" ]; then
    # openMSX's console needs a monospaced TrueType font
    FONT=$(fc-list :spacing=mono file 2>/dev/null | grep -i '\.ttf' | head -1 | sed 's/: *$//')
    [ -n "$FONT" ] || FONT=$(fc-list : file 2>/dev/null | grep -i 'mono.*\.ttf' | head -1 | sed 's/: *$//')
    cat > "$OPENMSX_HOME/.openMSX/share/settings.xml" <<EOF
<!DOCTYPE settings SYSTEM 'settings.dtd'>
<settings>
  <settings>
    <setting id="renderer">SDL</setting>
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
EOF
fi

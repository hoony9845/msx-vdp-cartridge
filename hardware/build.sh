#!/bin/sh
# Regenerate the whole KiCad project: schematic -> netlist -> PCB -> route -> zones -> outputs
#
#   ./build.sh                          Pimoroni Pico Plus 2 version  -> msx_v9990_cart/, fab/
#   MSX_VARIANT=core2350b ./build.sh    Waveshare Core2350B version   -> msx_v9990_cart_core2350b/, fab_core2350b/
set -e
cd "$(dirname "$0")"
export MSX_VARIANT=${MSX_VARIANT:-picoplus2}
if [ "$MSX_VARIANT" = core2350b ]; then
    P=msx_v9990_cart_core2350b; FAB=fab_core2350b; REP=reports_core2350b
else
    P=msx_v9990_cart; FAB=fab; REP=reports
fi
python3 gen/gen_project.py $P
python3 gen/gen_schematic.py $P
kicad-cli sch export netlist --format kicadsexpr -o $P/$P.net $P/$P.kicad_sch >/dev/null
(cd gen && PYTHONPATH= python3 gen_pcb.py ../$P ../$P/$P.net)
(cd gen && FR_ARGS="${FR_ARGS:--us hybrid -hr 1:1}" PYTHONPATH= python3 route.py ../$P ${PASSES:-150})
(cd gen && PYTHONPATH= python3 finish_pcb.py ../$P)

# Checks: ERC, DRC with schematic parity (fail on any error)
mkdir -p $REP
kicad-cli sch erc --severity-all --exit-code-violations -o $REP/erc.rpt $P/$P.kicad_sch >/dev/null
kicad-cli pcb drc --schematic-parity --severity-all --exit-code-violations -o $REP/drc.rpt $P/$P.kicad_pcb >/dev/null
grep -E "ERC messages" $REP/erc.rpt
grep -E "^\*\* Found" $REP/drc.rpt

# Fabrication outputs
(cd gen && python3 export_fab.py ../$P ../$FAB)
(cd gen && PYTHONPATH= python3 export_hole_check.py ../$P ../$FAB/${P}_hole_check.pdf)
# copy compensated for a printer that shrinks to 96% (see gen/scale_pdf.sh)
sh gen/scale_pdf.sh $FAB/${P}_hole_check.pdf $FAB/${P}_hole_check_print96.pdf 96 >/dev/null

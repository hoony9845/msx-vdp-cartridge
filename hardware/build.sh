#!/bin/sh
# Regenerate the whole KiCad project: schematic -> netlist -> PCB -> route -> zones -> outputs
set -e
cd "$(dirname "$0")"
P=msx_v9990_cart
python3 gen/gen_project.py $P
python3 gen/gen_schematic.py $P
kicad-cli sch export netlist --format kicadsexpr -o $P/$P.net $P/$P.kicad_sch >/dev/null
(cd gen && PYTHONPATH= python3 gen_pcb.py ../$P ../$P/$P.net)
(cd gen && FR_ARGS="${FR_ARGS:--us hybrid -hr 1:1}" PYTHONPATH= python3 route.py ../$P ${PASSES:-150})
(cd gen && PYTHONPATH= python3 finish_pcb.py ../$P)

# Checks: ERC, DRC with schematic parity (fail on any error)
mkdir -p reports
kicad-cli sch erc --severity-all --exit-code-violations -o reports/erc.rpt $P/$P.kicad_sch >/dev/null
kicad-cli pcb drc --schematic-parity --severity-all --exit-code-violations -o reports/drc.rpt $P/$P.kicad_pcb >/dev/null
grep -E "ERC messages" reports/erc.rpt
grep -E "^\*\* Found" reports/drc.rpt

# Fabrication outputs
(cd gen && python3 export_fab.py ../$P ../fab)
(cd gen && PYTHONPATH= python3 export_hole_check.py ../$P ../fab/${P}_hole_check.pdf)
# copy compensated for a printer that shrinks to 96% (see gen/scale_pdf.sh)
sh gen/scale_pdf.sh fab/${P}_hole_check.pdf fab/${P}_hole_check_print96.pdf 96 >/dev/null

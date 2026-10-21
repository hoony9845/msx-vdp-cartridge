#!/bin/sh
# Compensate a printer that shrinks the page: scale the PDF content about the
# top-left corner, keeping the A4 landscape page size.
#
#   scale_pdf.sh <in.pdf> <out.pdf> <measured_x_mm> [measured_y_mm]
#
# measured_*_mm = what the printed 100 mm ruler (x) / 60 mm ruler scaled to
# 100 (y) actually measured. Example: the 100 mm ruler printed as 96 mm ->
#   scale_pdf.sh hole_check.pdf hole_check_comp.pdf 96
set -e
IN=$1; OUT=$2; MX=$3; MY=${4:-$3}
SX=$(python3 -c "print(100.0/$MX)")
SY=$(python3 -c "print(100.0/$MY)")
W=841.896; H=595.296      # A4 landscape in points
gs -q -o "$OUT" -sDEVICE=pdfwrite -dDEVICEWIDTHPOINTS=$W -dDEVICEHEIGHTPOINTS=$H -dFIXEDMEDIA \
   -dAutoRotatePages=/None \
   -c "<</BeginPage {0 $H translate $SX $SY scale 0 -$H translate}>> setpagedevice" \
   -f "$IN"
echo "scaled x$SX / y$SY -> $OUT"

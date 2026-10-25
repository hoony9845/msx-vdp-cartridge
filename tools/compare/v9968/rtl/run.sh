#!/bin/sh
# Compare core/v9968_cmd.c with HRA!'s vdp_command.v (iverilog) on random commands.
#   run.sh <seed> <mode 0-9> <v256> <hs> <n>
# Needs: HRA_V9968=<clone of hra1129/V9968_Cartridge>, iverilog/vvp in PATH,
#        build-host/v9968_cmd_replay. Work files go to ./run_<seed>_<mode>_<v256>_<hs>.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
V=${HRA_V9968:?set HRA_V9968}/fpga/V9968_Cartridge_TangNano20K/src/v9968
[ -f "$HERE/tb.vvp" ] || iverilog -g2012 -o "$HERE/tb.vvp" "$HERE/tb_cmd.v" "$V/vdp_command.v" "$V/vdp_command_cache.v"
seed=$1 mode=$2 v256=$3 hs=$4 n=$5
il=0; [ $mode -eq 5 -o $mode -eq 6 ] && il=1
d=run_${seed}_${mode}_${v256}_${hs}; mkdir -p $d; cd $d
python3 "$HERE/gen_cmds.py" $seed $mode $v256 $n > cmds.txt
tbc=$((seed % 256))
timeout 600 vvp -n "$HERE/tb.vvp" +mode=$mode +il=$il +v256=$v256 +hs=$hs +tbc=$tbc > /dev/null
timeout 120 "$ROOT/build-host/v9968_cmd_replay" $mode $v256 $hs $tbc init.hex cmds.txt c_dump.hex c_log.txt
if cmp -s rtl_dump.hex c_dump.hex && cmp -s rtl_log.txt c_log.txt; then
    echo "seed $seed mode $mode v256 $v256 hs $hs: identical"
else
    echo "seed $seed mode $mode v256 $v256 hs $hs: DIFF"
fi

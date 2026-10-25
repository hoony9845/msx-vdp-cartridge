#!/usr/bin/env python3
"""Find the first command after which the RTL and core/v9968_cmd.c disagree.

    rtl_bisect.py <run_dir> <mode> <v256> <hs> <R#12>

run_dir is a directory made by run.sh (cmds.txt, init.hex). Prints the
command (index and registers) after which the VRAM or the log first differ.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../../.."))
run, mode, v256, hs, r12 = sys.argv[1], *sys.argv[2:6]
il = "1" if mode in ("5", "6") else "0"
lines = open(os.path.join(run, "cmds.txt")).read().split("\n")
ends = [i for i, l in enumerate(lines) if l == "E"]


def same(k):
    cut = os.path.join(run, "cut.txt")
    open(cut, "w").write("\n".join(lines[:ends[k] + 1]) + "\n")
    subprocess.run(["vvp", "-n", os.path.join(HERE, "tb.vvp"), f"+mode={mode}", f"+il={il}",
                    f"+v256={v256}", f"+hs={hs}", f"+tbc={r12}", f"+cmds={cut}",
                    f"+init={run}/init.hex", f"+dump={run}/cut_rtl.hex", f"+log={run}/cut_rtl.log"],
                   stdout=subprocess.DEVNULL, check=True)
    subprocess.run([os.path.join(ROOT, "build-host/v9968_cmd_replay"), mode, v256, hs, r12,
                    f"{run}/init.hex", cut, f"{run}/cut_c.hex", f"{run}/cut_c.log"], check=True)
    return (open(f"{run}/cut_rtl.hex").read() == open(f"{run}/cut_c.hex").read() and
            open(f"{run}/cut_rtl.log").read() == open(f"{run}/cut_c.log").read())


lo, hi = 0, len(ends) - 1
if same(hi):
    print("no difference")
    sys.exit(0)
while lo < hi:
    m = (lo + hi) // 2
    if same(m):
        lo = m + 1
    else:
        hi = m
regs = {}
start = ends[lo - 1] + 1 if lo else 0
for l in lines[start:ends[lo]]:
    p = l.split()
    if p and p[0] == "W":
        regs[int(p[1])] = int(p[2], 16)
names = {15: "HMMC", 14: "YMMM", 13: "HMMM", 12: "HMMV", 11: "LMMC", 10: "LMCM", 9: "LMMM",
         8: "LMMV", 7: "LINE", 6: "SRCH", 5: "PSET", 4: "POINT", 3: "LRMM", 2: "LFMC", 1: "LFMM", 0: "STOP"}
op = regs.get(46, 0)
print(f"first difference after command #{lo + 1}: {names[op >> 4]} lop {op & 15}",
      {k: hex(v) for k, v in sorted(regs.items())})

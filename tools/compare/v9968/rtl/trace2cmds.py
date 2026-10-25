#!/usr/bin/env python3
"""Turn the command part of a V9968 scenario trace into tb_cmd.v input.

    trace2cmds.py <trace.txt> <out_dir> [il]

Writes <out_dir>/cmds.txt (W/E lines) and <out_dir>/init.hex (VRAM written
through port 0 among the commands; with "il", mapped to the G6/G7
interleaved layout like the VDP stores it). Only what follows the last write to R#0 is taken (the
scenario's own setup; the reset before it only clears VRAM).
"""
import os
import sys

lines = [l.split() for l in open(sys.argv[1]) if l.startswith("w ")]
writes = [(int(a, 16) & 7, int(v, 16)) for _, a, v in lines]

latch, second, ptr, noinc_ptr = 0, False, 0, False
addr, noinc, r14 = 0, False, 0
events, vram = [], {}
last_r0 = 0
for port, v in writes:
    if port == 1:
        if not second:
            latch, second = v, True
            continue
        second = False
        if v & 0x80:
            events.append(("reg", v & 0x3F, latch))
        else:
            addr = (r14 << 14) | ((v & 0x3F) << 8) | latch
            noinc = bool(v & 0x40)
    elif port == 3:
        events.append(("reg", ptr, v))
        if not noinc_ptr:
            ptr = (ptr + 1) & 0x3F
    elif port == 0:
        second = False
        if noinc:
            noinc = False
        else:
            addr = (addr + 1) & 0x3FFFF
        events.append(("vram", addr, v))
    if events and events[-1][0] == "reg":
        _, r, d = events[-1]
        if r == 17:
            ptr, noinc_ptr = d & 0x3F, bool(d & 0x80)
        if r == 14:
            r14 = d & 0x0F
        if r == 0:
            last_r0 = len(events)

out = sys.argv[2]
os.makedirs(out, exist_ok=True)
cmds = []
for kind, a, d in events[last_r0:]:
    if kind == "vram":
        if len(sys.argv) > 3 and sys.argv[3] == "il":
            a = (a & 0x20000) | ((a & 1) << 16) | ((a >> 1) & 0xFFFF)
        vram[a] = d
    elif a >= 32:
        cmds.append(f"W {a} {d:02x}")
        if a == 46:
            cmds.append("E")
open(os.path.join(out, "cmds.txt"), "w").write("\n".join(cmds) + "\n")
open(os.path.join(out, "init.hex"), "w").write(
    "\n".join(f"{vram.get(i, 0):02x}" for i in range(0x40000)) + "\n")
print(len(cmds), "command register writes")

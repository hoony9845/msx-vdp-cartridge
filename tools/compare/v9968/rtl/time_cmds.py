#!/usr/bin/env python3
"""Commands to time on the RTL (tb_time.v) and the timing model
(v9968_cmd_replay with V9968_LOG_CLOCKS): writes cmds.txt and names.txt
(name, pixels per command) for one screen mode.
  time_cmds.py <out dir> <mode: 3 = G4, 6 = G7>"""
import os, random, sys

out, mode = sys.argv[1], int(sys.argv[2])
H = 16 if mode == 3 else 8           # band height (bytes 0..2047)

def regs(d):
    return [(r, v) for r, v in d]

def rect(op, nx, ny, sx=None, sy=None, dx=0, dy=0, clr=0x5A, arg=0):
    w = []
    if sx is not None:
        w += [(32, sx & 255), (33, sx >> 8), (34, sy & 255), (35, sy >> 8)]
    w += [(36, dx & 255), (37, dx >> 8), (38, dy & 255), (39, dy >> 8),
          (40, nx & 255), (41, nx >> 8), (42, ny & 255), (43, ny >> 8),
          (44, clr), (45, arg), (46, op)]
    return w

def lrmm(nx, ny, lop=0):
    return [(32, 0), (33, 0), (34, 0), (35, 0), (36, 0), (37, 0), (38, 0), (39, 0),
            (40, nx), (41, 0), (42, ny), (43, 0), (44, 0x5A), (45, 0),
            (47, 0), (48, 1), (49, 0), (50, 0),
            (51, 0), (52, 0), (53, 0), (54, 0), (55, 0xFF), (56, 1), (57, 0xFF), (58, 7),
            (46, 0x30 | lop)]

cmds = [
    ("HMMV 128xH", rect(0xC0, 128, H), 128 * H),
    ("LMMV 128xH", rect(0x80, 128, H), 128 * H),
    ("LMMV TIMP 128xH", rect(0x88, 128, H), 128 * H),
    ("HMMM 128xH", rect(0xD0, 128, H, 128, 64), 128 * H),
    ("LMMM 128xH", rect(0x90, 128, H, 128, 64), 128 * H),
    ("LMMM TIMP 128xH", rect(0x98, 128, H, 128, 64), 128 * H),
    ("YMMM 128xH", [(34, 64), (35, 0)] + rect(0xE0, 0, H, dx=128), 128 * H),
    ("LINE 128", rect(0x70, 127, H - 1), 128),
    ("LINE 16", rect(0x70, 15, 3), 16),
    ("LINE 16 horizontal", rect(0x70, 15, 0), 16),
    ("PSET", rect(0x50, 0, 0, dx=5, dy=5), 1),
    ("POINT", [(32, 5), (33, 0), (34, 5), (35, 0), (46, 0x40)], 1),
    ("LRMM 128xH", lrmm(128, H), 128 * H),
    ("LRMM TIMP 128xH", lrmm(128, H, 8), 128 * H),
    ("LRMM 16x1", lrmm(16, 1), 16),
    ("LRMM TIMP 16x1", lrmm(16, 1, 8), 16),
]
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "cmds.txt"), "w") as f, open(os.path.join(out, "names.txt"), "w") as n:
    for name, w, px in cmds:
        for rep in range(2):          # the second one runs with a warm cache
            for r, v in w:
                f.write("W %d %02x\n" % (r, v))
            f.write("E\n")
            n.write("%s|%d|%d\n" % (name.replace("H", str(H)) if "xH" in name else name, px, rep))
random.seed(1)
with open(os.path.join(out, "init.hex"), "w") as f:
    for i in range(262144):
        f.write("%02x\n" % random.randrange(256))

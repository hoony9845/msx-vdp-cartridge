#!/usr/bin/env python3
"""Random command-engine stimulus for tb_cmd.v and v9968_cmd_replay.
usage: gen_cmds.py <seed> <mode> <v256> <n> > cmds.txt ; writes init.hex"""
import random, sys

seed, mode, v256, n = (int(a) for a in sys.argv[1:5])
rng = random.Random(seed)
with open("init.hex", "w") as f:
    f.write("\n".join("%02x" % rng.randrange(256) for _ in range(262144)) + "\n")

wide = mode in (4, 5)
W = 512 if wide else 256
H = 2048 if v256 else 1024
out = []
def w(r, v): out.append("W %d %02x" % (r, v & 0xFF))

def regs(sx, sy, dx, dy, nx, ny, clr, arg):
    for r, v in ((32, sx), (33, sx >> 8), (34, sy), (35, sy >> 8), (36, dx), (37, dx >> 8),
                 (38, dy), (39, dy >> 8), (40, nx), (41, nx >> 8), (42, ny), (43, ny >> 8),
                 (44, clr), (45, arg)):
        w(r, v)

ops = ["HMMC", "YMMM", "HMMM", "HMMV", "LMMC", "LMCM", "LMMM", "LMMV",
       "LINE", "SRCH", "PSET", "POINT"]
if v256:
    ops += ["LRMM", "LRMM", "LRMM", "LFMC", "LFMM"]
code = {"HMMC": 15, "YMMM": 14, "HMMM": 13, "HMMV": 12, "LMMC": 11, "LMCM": 10, "LMMM": 9,
        "LMMV": 8, "LINE": 7, "SRCH": 6, "PSET": 5, "POINT": 4, "LRMM": 3, "LFMC": 2, "LFMM": 1}
for k in range(n):
    op = rng.choice(ops)
    dix, diy = rng.randrange(2), rng.randrange(2)
    ext = (rng.randrange(4) << 6) if v256 and rng.random() < 0.3 else 0
    if op == "LRMM" and rng.random() < 0.3:
        ext |= 0x40                                   # XHR
    lop = rng.randrange(16)
    clr = rng.choice([rng.randrange(256), rng.randrange(16), rng.randrange(4), 0])
    sx = rng.randrange(W) if rng.random() < 0.9 else rng.randrange(4096)
    sy = rng.randrange(H)
    dx = rng.randrange(W)
    dy = rng.randrange(H)
    nx = rng.choice([rng.randrange(1, 40), rng.randrange(1, 300), 0, 1, 2, 3])
    ny = rng.choice([rng.randrange(1, 12), rng.randrange(1, 40), 1])
    if op in ("LINE",):
        nx = rng.randrange(0, 300)
        ny = rng.randrange(0, nx + 1)
        arg = ext | (diy << 3) | (dix << 2) | rng.randrange(2)
    elif op == "SRCH":
        arg = ext | (dix << 2) | (rng.randrange(2) << 1)
        clr = rng.randrange(16) if mode in (3, 5) else rng.randrange(4) if mode == 4 else rng.randrange(256)
    else:
        arg = ext | (diy << 3) | (dix << 2)
    if op in ("LMCM", "LMMC", "HMMC"):
        nx = rng.randrange(1, 24)
        ny = rng.randrange(1, 6)
    if op == "LFMC":
        nx = rng.randrange(1, 9)
        ny = rng.randrange(1, 10)
    if op == "LFMM":
        # DX must not leave the screen: the RTL never ends then (NY only
        # advances on bit 0), so the hardware would hang as well
        nx = rng.randrange(1, 30)
        ny = rng.randrange(1, 17)
        dix = 0
        dx = rng.randrange(0, W - 8 * min(nx, W // 8 - 1))
        nx = 1          # NX > 1 never ends in the RTL either (NX is overwritten)
        arg = ext | (diy << 3)
        fa = rng.randrange(262144)
        sx, sy = fa & 0xFFFF, (fa >> 16) & 3
        sy = (sy) | (rng.randrange(64) << 2)
    if op == "LRMM":
        sx = rng.randrange(4096)
        sy = rng.randrange(8192)
        nx = rng.randrange(1, 300)
        ny = rng.randrange(1, 30)
        vx, vy = rng.randrange(65536), rng.randrange(65536)
        if rng.random() < 0.5:
            vx, vy = rng.choice([256, 128, 512, 384]), rng.choice([0, 0, 32, 65536 - 48])
        for r, v in ((47, vx), (48, vx >> 8), (49, vy), (50, vy >> 8)):
            w(r, v)
        win = (0, 511, 0, 2047)
        if rng.random() < 0.4:
            wsx, wsy = rng.randrange(512), rng.randrange(2048)
            win = (wsx, min(511, wsx + rng.randrange(300)), wsy, min(2047, wsy + rng.randrange(400)))
        for r, v in ((51, win[0]), (52, win[0] >> 8), (53, win[2]), (54, win[2] >> 8),
                     (55, win[1]), (56, win[1] >> 8), (57, win[3]), (58, win[3] >> 8)):
            w(r, v)
    regs(sx, sy, dx, dy, nx, ny, clr, arg)
    w(46, (code[op] << 4) | lop)
    if op in ("LMMC", "HMMC"):
        for _ in range(nx * ny + 4):
            out.append("T %02x" % rng.randrange(256))
    elif op == "LFMC":
        for _ in range(ny + 4):
            out.append("T %02x" % rng.randrange(256))
    elif op == "LMCM":
        for _ in range(nx * ny + 4):
            out.append("S")
    out.append("E")
print("\n".join(out))

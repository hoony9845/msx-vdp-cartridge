"""
Generate V9968 test scenarios as port traces (see host/replay9968.c for the
format). Each one runs in the openMSX V9968 fork and in our core; compare.py
(one directory up) compares the screenshots dot by dot.

    python3 scenarios.py <out_dir> [name ...]
"""
import math
import os
import random
import sys


class Trace:
    def __init__(self):
        self.lines = []

    # --- raw port access -------------------------------------------------
    def out(self, port, value):
        self.lines.append(f"w 0x{0x88 | (port & 7):02X} 0x{value & 0xFF:02X}")

    def inp(self, port):
        self.lines.append(f"r 0x{0x88 | (port & 7):02X}")

    def frame(self, n=1):
        self.lines += ["frame"] * n

    def shot(self, name):
        self.lines.append(f"shot {name}")

    def wait_ce(self):
        self.lines.append("wait_ce")

    # --- VDP helpers (what an MSX program would do) -----------------------
    def reg(self, r, value):
        self.out(1, value)
        self.out(1, 0x80 | r)

    def regs(self, first, values):
        self.reg(17, first)
        for v in values:
            self.out(3, v)

    def vram_addr(self, addr):
        self.reg(14, (addr >> 14) & 0x0F)
        self.out(1, addr & 0xFF)
        self.out(1, 0x40 | ((addr >> 8) & 0x3F))

    def vram_write(self, addr, data):
        self.vram_addr(addr)
        for b in data:
            self.out(0, b)

    def palette(self, index, r, g, b):
        self.reg(16, index)
        self.out(2, (r << 4) | b)
        self.out(2, g)

    def palette15(self, index, r, g, b):
        self.reg(16, index)
        self.out(2, r)
        self.out(2, g)
        self.out(2, b)

    def ext(self, r20, v9968=True):
        """Unlock R#20/R#21 (port 4 bit7 = 0), select the V9968 or V9958 mode."""
        self.out(4, 0x00)
        self.reg(21, 0 if v9968 else 1)
        self.reg(20, r20)

    def cmd(self, op, sx=0, sy=0, dx=0, dy=0, nx=0, ny=0, clr=0, arg=0, lop=0):
        self.regs(32, [sx & 0xFF, sx >> 8, sy & 0xFF, sy >> 8, dx & 0xFF, dx >> 8,
                       dy & 0xFF, dy >> 8, nx & 0xFF, nx >> 8, ny & 0xFF, ny >> 8,
                       clr & 0xFF, arg, (op << 4) | lop])
        self.wait_ce()

    def save(self, path):
        with open(path, "w") as f:
            f.write("\n".join(self.lines) + "\n")


HMMV, HMMM, YMMM, LMMV, LMMM, LINE, PSET, LRMM, LFMM = 0xC, 0xD, 0xE, 0x8, 0x9, 0x7, 0x5, 0x3, 0x1

MSX2_PALETTE = [(0, 0, 0), (0, 0, 0), (1, 6, 1), (3, 7, 3), (1, 1, 7), (2, 3, 7), (5, 1, 1), (2, 6, 7),
                (7, 1, 1), (7, 3, 3), (6, 6, 1), (6, 6, 4), (1, 4, 1), (6, 2, 5), (5, 5, 5), (7, 7, 7)]

# {M5..M1} -> (R#0, R#1 mode bits)
MODES = {"G1": (0x00, 0x00), "G2": (0x02, 0x00), "G3": (0x04, 0x00), "G4": (0x06, 0x00),
         "G5": (0x08, 0x00), "G6": (0x0A, 0x00), "G7": (0x0E, 0x00), "T1": (0x00, 0x10),
         "T2": (0x04, 0x10), "MC": (0x00, 0x08)}


def reset(t):
    """Known state: V9958 mode, VRAM cleared (256KB), MSX2 palette."""
    t.ext(0x00, v9968=True)
    t.reg(0, 0x0E)                      # GRAPHIC7
    t.reg(1, 0x00)                      # display off, no interrupts
    t.reg(8, 0x0A)                      # VR, sprites off
    t.reg(9, 0x80)
    for r in (18, 23, 25, 26, 27):
        t.reg(r, 0)
    t.cmd(HMMV, dx=0, dy=0, nx=256, ny=1024, clr=0)
    t.reg(21, 1)                        # back to V9958
    t.out(4, 0x80)                      # lock R#20/R#21 again
    for i, c in enumerate(MSX2_PALETTE):
        t.palette(i, *c)


def screen(t, mode, lines212=True, sprites=True, r1_extra=0, tables=None):
    r0, r1 = MODES[mode]
    t.reg(0, r0)
    t.reg(1, 0x40 | r1 | r1_extra)
    t.reg(8, 0x08 | (0 if sprites else 0x02))
    t.reg(9, 0x80 if lines212 else 0x00)
    for r, v in (tables or {}).items():
        t.reg(r, v)
    t.frame()


def font8(c, r):
    """A distinct 8x8 glyph for every character code."""
    v = ((c * 0x9D) ^ (r * 0x35) ^ (c >> 3)) & 0xFF
    return v | (0x80 >> (c & 7)) if r == (c & 7) else v


# ---------------------------------------------------------------------------
# calibration (compare.py locates the picture with these)

def calibration():
    t = Trace()
    reset(t)
    screen(t, "G4", sprites=False, tables={2: 0x1F, 7: 0x00})
    t.palette(15, 7, 7, 7)
    t.cmd(HMMV, dx=0, dy=0, nx=256, ny=212, clr=0xFF)
    t.frame(2)
    t.shot("calibration")
    return t


def calib_dots(wide):
    t = Trace()
    reset(t)
    screen(t, "G6" if wide else "G4", sprites=False, tables={2: 0x1F, 7: 0x00})
    t.palette(15, 7, 7, 7)
    t.cmd(HMMV, dx=0, dy=0, nx=512 if wide else 256, ny=212, clr=0xF0)
    t.frame(2)
    t.shot("calib_dots_512" if wide else "calib_dots_256")
    return t


ROW_COLORS = [(7, 0, 0), (0, 7, 0), (0, 0, 7), (7, 7, 0), (0, 7, 7), (7, 0, 7), (7, 7, 7)]


def calib_rows():
    t = Trace()
    reset(t)
    screen(t, "G4", sprites=False, tables={2: 0x1F, 7: 0x00})
    for i, c in enumerate(ROW_COLORS):
        t.palette(i + 1, *c)
    for base in (0, 200):
        for y in range(len(ROW_COLORS)):
            t.cmd(HMMV, dx=0, dy=base + y, nx=256, ny=1, clr=(y + 1) * 0x11)
    t.frame(2)
    t.shot("calib_rows_256x212")
    return t


# ---------------------------------------------------------------------------
# text and pattern modes

def text1():
    t = Trace()
    reset(t)
    screen(t, "T1", sprites=False, tables={2: 0x00, 4: 0x01, 7: 0xF4})
    t.vram_write(0x800, [font8(c, r) for c in range(256) for r in range(8)])
    t.vram_write(0x000, [(i * 7) & 0xFF for i in range(960)])
    t.frame(2)
    t.shot("text1")
    return t


def text2():
    t = Trace()
    reset(t)
    screen(t, "T2", sprites=False, tables={2: 0x03, 3: 0x2F, 10: 0x00, 4: 0x02, 7: 0xE1, 12: 0x48})
    t.vram_write(0x1000, [font8(c, r) for c in range(256) for r in range(8)])
    t.vram_write(0x000, [(i * 13 + 5) & 0xFF for i in range(80 * 24)])
    t.frame(2)
    t.shot("text2")
    return t


def sprite_tables(t, sat, spg, mode2, n=32, seed=1, big=False, sct=None):
    rng = random.Random(seed)
    pats = []
    for p in range(64):
        pats += [rng.randrange(256) | 0x81 for _ in range(8)]
    t.vram_write(spg, pats * 4)
    attrs = []
    for p in range(32):
        y = rng.randrange(0, 200) if p < n else 216 if mode2 else 208
        x = rng.randrange(256)
        pat = rng.randrange(64) * (4 if big else 1)
        col = rng.randrange(1, 16) | (0x80 if rng.random() < 0.15 else 0)
        attrs += [y, x, pat & 0xFF, col]
    t.vram_write(sat, attrs)
    if mode2:
        cols = []
        for p in range(32):
            for r in range(16):
                c = rng.randrange(1, 16)
                if p % 5 == 3:
                    c |= 0x40                               # CC
                if p % 7 == 2:
                    c |= 0x20                               # IC
                if r == 0 and p % 9 == 4:
                    c |= 0x80                               # EC
                cols.append(c)
        t.vram_write(sct, cols)


def graphic1():
    t = Trace()
    reset(t)
    screen(t, "G1", tables={2: 0x06, 3: 0x80, 10: 0, 4: 0x00, 5: 0x36, 11: 0, 6: 0x07, 7: 0x05})
    t.vram_write(0x0000, [font8(c, r) for c in range(256) for r in range(8)])
    t.vram_write(0x2000, [((i * 0x37) & 0xF0) | ((i * 5 + 1) & 0x0F) for i in range(32)])
    t.vram_write(0x1800, [(i * 3) & 0xFF for i in range(768)])
    sprite_tables(t, 0x1B00, 0x3800, False, n=24, seed=2)
    # five sprites on one line: the fifth disappears
    t.vram_write(0x1B00, sum(([100, 20 + 30 * p, p, 15] for p in range(5)), []))
    t.frame(2)
    t.shot("graphic1")
    return t


def graphic2():
    t = Trace()
    reset(t)
    screen(t, "G2", r1_extra=0x03, tables={2: 0x06, 3: 0xFF, 10: 0, 4: 0x03, 5: 0x36, 11: 0,
                                         6: 0x07, 7: 0x01})
    for third in range(3):
        t.vram_write(0x0000 + third * 0x800, [font8(c + third, r) for c in range(256) for r in range(8)])
        t.vram_write(0x2000 + third * 0x800, [((c * 0x47 + r * 0x13 + third) | 0x11) & 0xFF
                                              for c in range(256) for r in range(8)])
    t.vram_write(0x1800, [(i * 5) & 0xFF for i in range(768)])
    sprite_tables(t, 0x1B00, 0x3800, False, n=12, seed=3, big=True)
    t.frame(2)
    t.shot("graphic2_mag16")
    return t


def multicolor():
    t = Trace()
    reset(t)
    screen(t, "MC", tables={2: 0x02, 4: 0x00, 5: 0x36, 11: 0, 6: 0x07, 7: 0x06})
    t.vram_write(0x0000, [(c * 0x1D + r * 0x51) & 0xFF for c in range(256) for r in range(8)])
    t.vram_write(0x0800, [(i * 7 + (i >> 5)) & 0xFF for i in range(768)])
    sprite_tables(t, 0x1B00, 0x3800, False, n=8, seed=4)
    t.frame(2)
    t.shot("multicolor")
    return t


def graphic3():
    t = Trace()
    reset(t)
    screen(t, "G3", r1_extra=0x02, tables={2: 0x06, 3: 0xFF, 10: 0, 4: 0x03, 5: 0x3F, 11: 0,
                                         6: 0x07, 7: 0x00})
    for third in range(3):
        t.vram_write(0x0000 + third * 0x800, [font8(c ^ third, r) for c in range(256) for r in range(8)])
        t.vram_write(0x2000 + third * 0x800, [((c * 0x2B + r * 0x17) | 0x21) & 0xFF
                                              for c in range(256) for r in range(8)])
    t.vram_write(0x1800, [(i * 11) & 0xFF for i in range(768)])
    sprite_tables(t, 0x1E00, 0x3800, True, n=32, seed=5, big=True, sct=0x1C00)
    t.frame(2)
    t.shot("graphic3_cc_ic")
    return t


# ---------------------------------------------------------------------------
# bitmap modes

def bitmap_fill(t, mode, seed, lines=212):
    rng = random.Random(seed)
    if mode in ("G4", "G5"):
        for y in range(0, lines, 2):
            t.cmd(HMMV, dx=0, dy=y, nx=512 if mode == "G5" else 256, ny=2, clr=rng.randrange(256))
        # diagonal stripes for orientation
        for k in range(16):
            t.cmd(LINE, dx=k * 12, dy=0, nx=200, ny=150, clr=k, arg=0x00)
    else:
        for y in range(0, lines, 3):
            t.cmd(HMMV, dx=0, dy=y, nx=512 if mode == "G6" else 256, ny=3, clr=rng.randrange(256))
        for k in range(16):
            t.cmd(LINE, dx=k * 12, dy=0, nx=200, ny=150, clr=k * 17 if mode == "G7" else k, arg=0x00)


def bitmap_scene(mode, name, sprites=True, tp=False, extra=None):
    t = Trace()
    reset(t)
    g67 = mode in ("G6", "G7")
    # SAT / colour table / patterns: 7600h/7400h/7800h, or FA00h/F800h/F000h in G6/G7
    screen(t, mode, sprites=sprites, r1_extra=0x02,
           tables={2: 0x1F, 5: 0xF7 if g67 else 0xEF, 11: 0x01 if g67 else 0x00,
                   6: 0x1E if g67 else 0x0F, 7: 0x04})
    if tp:
        t.reg(8, 0x28)
    bitmap_fill(t, mode, sum(map(ord, name)))
    if sprites:
        sat = 0xFA00 if g67 else 0x7600
        spg = 0xF000 if g67 else 0x7800
        sprite_tables(t, sat, spg, True, n=28, seed=len(name), big=True, sct=sat - 0x200)
    if extra:
        extra(t)
    t.frame(2)
    t.shot(name)
    return t


def yjk_scene(yae):
    t = Trace()
    reset(t)
    screen(t, "G7", sprites=True, r1_extra=0x02, tables={2: 0x1F, 5: 0xF7, 11: 0x01, 6: 0x1E, 7: 0x03})
    t.reg(25, 0x18 if yae else 0x08)
    # rows: Y ramps, J/K vary per 4-dot group
    for y in range(0, 212, 4):
        row = []
        for g in range(64):
            j = ((g * 3) % 64) - 32
            k = ((y // 4 * 5) % 64) - 32
            yy = (g // 2 + y // 8) & 31
            jb, kb = j & 63, k & 63
            px = [kb & 7, kb >> 3, jb & 7, jb >> 3]
            for i in range(4):
                b = (yy << 3) | px[i]
                if yae:
                    b = ((yy >> 1) << 4) | (0x08 if (g + y // 4) % 6 == 0 else 0) | px[i]
                row.append(b)
        for dy in range(4):
            t.vram_write((y + dy) * 256, row)            # (interleave handled by the VDP)
    sprite_tables(t, 0xFA00, 0xF000, True, n=8, seed=9, big=True, sct=0xF800)
    t.frame(2)
    t.shot("yae" if yae else "yjk")
    return t


def hscroll(sp2):
    def extra(t):
        t.reg(26, 0x05 if not sp2 else 0x23)
        t.reg(27, 3)
        t.reg(25, 0x02 | (0x01 if sp2 else 0))
        if sp2:
            t.reg(2, 0x3F)                    # pages 0/1 side by side
            t.cmd(HMMV, dx=0, dy=256, nx=256, ny=212, clr=0x5A)
            t.cmd(LINE, dx=10, dy=256, nx=230, ny=200, clr=0x0F)
    return bitmap_scene("G4", "hscroll_sp2" if sp2 else "hscroll_msk", extra=extra)


def vscroll():
    def extra(t):
        t.reg(23, 37)
    return bitmap_scene("G4", "vscroll_r23", extra=extra)


def adjust():
    def extra(t):
        t.reg(18, 0x30)
    return bitmap_scene("G4", "display_adjust", extra=extra)


# ---------------------------------------------------------------------------
# V9968 features

def epal():
    t = Trace()
    reset(t)
    t.ext(0x10)                               # V9968 mode, EPAL
    screen(t, "G7", sprites=False, tables={2: 0x1F, 7: 0x80})
    for i in range(256):
        r, g, b = (i * 7) & 31, (i >> 3) & 31, (31 - (i & 31))
        t.palette15(i, r, g, b)
    for y in range(0, 212, 2):
        t.cmd(HMMV, dx=0, dy=y, nx=256, ny=2, clr=y)
    for k in range(16):
        t.cmd(LINE, dx=k * 16, dy=0, nx=211, ny=100, clr=255 - k * 16)
    t.frame(2)
    t.shot("v9968_epal")
    return t


def sp3_scene():
    t = Trace()
    reset(t)
    t.ext(0x08 | 0x10)                        # V9968 mode, SP3, EPAL
    screen(t, "G4", tables={2: 0x1F, 5: 0xFC, 11: 0x00, 6: 0x20, 7: 0x02})
    bitmap_fill(t, "G4", 77)
    for i in range(16, 256):
        t.palette15(i, (i * 5) & 31, (i * 3) & 31, (i * 11) & 31)
    # sprite patterns: a GRAPHIC4 page at 0x10000 (page 2), drawn with commands
    for k in range(32):
        t.cmd(LINE, dx=k * 4, dy=512 + (k & 7), nx=60, ny=60, clr=(k % 15) + 1)
    t.cmd(LMMV, dx=128, dy=512, nx=64, ny=64, clr=9, lop=0)
    t.cmd(LMMV, dx=136, dy=520, nx=16, ny=16, clr=0)
    rng = random.Random(33)
    attrs = []
    for p in range(64):
        if p < 40:
            y = rng.randrange(0, 190)
            bs = rng.randrange(4)
            mgy = rng.choice([0, 16, 24, 32, 48, 64, 100])
            ctl = rng.randrange(16) | (rng.randrange(4) << 4) | (rng.randrange(4) << 6)
            x = rng.randrange(-30, 256) & 0x3FF
            page = 2
            mgx = rng.choice([8, 16, 24, 32, 48, 64, 128])
            pat = rng.choice([0x00, 0x01, 0x08, 0x10, 0x11, 0x23])
        else:
            y, bs, mgy, ctl, x, page, mgx, pat = 216, 0, 0, 0, 0, 0, 0, 0
        attrs += [y & 0xFF, ((y >> 8) & 3) | (bs << 6), mgy, ctl,
                  x & 0xFF, ((x >> 8) & 3) | (page << 4), mgx, pat]
    t.vram_write(0x7E00, attrs)
    t.frame(2)
    t.shot("v9968_sp3")
    return t


def s16_scene():
    def extra(t):
        t.ext(0x80)                           # S16 (V9958 mode is enough? use V9968)
        t.vram_write(0x7600, sum(([100, 8 + 16 * p, (p * 4) & 0xFF, 0] for p in range(14)), []) +
                     [216, 0, 0, 0])
    return bitmap_scene("G4", "v9968_s16", extra=extra)


def commands_scene():
    t = Trace()
    reset(t)
    t.ext(0x01)                               # V9968 mode, high speed
    screen(t, "G4", sprites=False, tables={2: 0x1F, 7: 0x01})
    bitmap_fill(t, "G4", 5)
    t.cmd(LMMV, dx=20, dy=20, nx=100, ny=50, clr=0x0A, lop=0x3)      # XOR
    t.cmd(LMMV, dx=40, dy=30, nx=100, ny=50, clr=0x00, lop=0x8)      # TIMP 0: nothing
    t.cmd(LMMV, dx=60, dy=40, nx=100, ny=50, clr=0x05, lop=0x2)      # OR
    t.cmd(LMMM, sx=0, sy=0, dx=150, dy=100, nx=90, ny=90, arg=0x00, lop=0x0)
    t.cmd(LMMM, sx=100, sy=100, dx=90, dy=200, nx=60, ny=40, arg=0x0C, lop=0x8)
    t.cmd(HMMM, sx=10, sy=150, dx=180, dy=10, nx=60, ny=60)
    t.cmd(YMMM, sy=20, dx=200, dy=160, ny=40)
    for k in range(24):
        a = k * math.pi / 12
        nx, ny = int(abs(90 * math.cos(a))), int(abs(90 * math.sin(a)))
        maj = ny > nx
        if maj:
            nx, ny = ny, nx
        t.cmd(LINE, dx=128, dy=106, nx=nx, ny=ny, clr=k % 16,
              arg=(0x04 if math.cos(a) < 0 else 0) | (0x08 if math.sin(a) < 0 else 0) | maj)
    t.frame(2)
    t.shot("v9968_commands")
    return t


def lrmm_scene():
    t = Trace()
    reset(t)
    t.ext(0x01)
    screen(t, "G4", sprites=False, tables={2: 0x1F, 7: 0x00})
    for y in range(64):                       # texture at page 2 (y = 512)
        t.cmd(HMMV, dx=0, dy=512 + y, nx=64, ny=1, clr=((y // 8) * 0x11) ^ 0x37)
    t.cmd(LINE, dx=0, dy=512, nx=63, ny=63, clr=15)
    for k, ang in enumerate((0, 20, 45, 90, 135, 200)):
        a = math.radians(ang)
        vx, vy = int(round(256 * math.cos(a))) & 0xFFFF, int(round(256 * math.sin(a))) & 0xFFFF
        t.regs(47, [vx & 0xFF, vx >> 8, vy & 0xFF, vy >> 8, 0, 0, 0, 2, 63, 0, 0xFF, 2])
        t.cmd(LRMM, sx=32, sy=512 + 32, dx=10 + (k % 3) * 80, dy=10 + (k // 3) * 100, nx=70, ny=70,
              clr=4, arg=0x00, lop=0)
    t.frame(2)
    t.shot("v9968_lrmm")
    return t


def lfmm_scene():
    t = Trace()
    reset(t)
    t.ext(0x01)
    screen(t, "G7", sprites=False, tables={2: 0x1F, 7: 0x00, 12: 0x24})
    font = [font8(c, r) for c in range(64) for r in range(8)]
    t.vram_write(0x30000, font)
    for k in range(6):
        fa = 0x30000 + k * 64
        t.regs(32, [fa & 0xFF, (fa >> 8) & 0xFF, (fa >> 16) & 3])
        t.regs(36, [8 + k * 40, 0, 20 + k * 20, 0, 4, 0, 8, 0, 0xFC - k * 32, 0x00, (LFMM << 4) | 0])
        t.wait_ce()
    t.frame(2)
    t.shot("v9968_lfmm")
    return t


# --- geo3d -------------------------------------------------------------------

def geo_words(t, idx, words):
    t.out(5, idx)
    for w in words:
        t.out(7, w & 0xFF)
        t.out(7, (w >> 8) & 0xFF)


def rot(ax, ay, az, s=1.0):
    cx, sx, cy, sy, cz, sz = math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay), math.cos(az), math.sin(az)
    rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    mul = lambda a, b: [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    r = mul(rz, mul(ry, rx))
    return [int(round(r[i][j] * s * 16384)) for i in range(3) for j in range(3)]


def box(cx, cy, cz, s):
    v = [(cx + x, cy + y, cz + z) for x in (-s, s) for y in (-s, s) for z in (-s, s)]
    return v


def geo3d_scene(faces, textured):
    t = Trace()
    reset(t)
    t.ext(0x01)                               # V9968 mode (LRMM), high speed
    screen(t, "G4", sprites=False, tables={2: 0x1F, 7: 0x00})
    for i in range(1, 16):
        t.palette(i, i // 2, (i * 3) % 8, 7 - i // 2)
    if textured:
        for y in range(64):
            for c in range(7):
                t.cmd(HMMV, dx=0, dy=512 + c * 64 + y, nx=64, ny=1,
                      clr=(((y // 8 + c) % 7 + 1) * 0x11) ^ (0x10 if y & 4 else 0))
    geo_words(t, 0x18, [256, 128, 106, 16, 256, 212])
    verts = box(-300, 0, 0, 200) + box(300, 0, 0, 200)
    edges = [(i + o, j + o) for o in (0, 8) for i in range(8) for j in range(i + 1, 8)
             if bin(i ^ j).count("1") == 1]
    quads = [(0, 1, 3, 2, -1, 0, 0), (4, 6, 7, 5, 1, 0, 0), (0, 4, 5, 1, 0, -1, 0),
             (2, 3, 7, 6, 0, 1, 0), (0, 2, 6, 4, 0, 0, -1), (1, 5, 7, 3, 0, 0, 1)]
    t.out(5, 0x40)
    for b in (0, 0, len(verts), len(edges), 15, 0, 0, 0):
        t.out(7, b)
    t.out(5, 0x50)
    for v in verts:
        for c in v:
            t.out(7, c & 0xFF)
            t.out(7, (c >> 8) & 0xFF)
    t.out(5, 0x51)
    for a, b in edges:
        t.out(7, a)
        t.out(7, b)
    if faces:
        t.out(5, 0x58)
        t.out(7, 0)
        t.out(7, 12)
        t.out(5, 0x52)
        for o in (0, 8):
            for q in quads:
                for k in range(4):
                    t.out(7, q[k] + o)
                for n in q[4:7]:
                    w = n * 16384
                    t.out(7, w & 0xFF)
                    t.out(7, (w >> 8) & 0xFF)
                t.out(7, (0x80 if textured and o == 8 else 0) | (1 if o == 0 else 8))
        if textured:
            geo_words(t, 0x60, [0, 512])
            t.out(5, 0x64)
            t.out(7, 64)
            t.out(5, 0x65)
            t.out(7, 0)
            t.out(5, 0x53)
            for f in range(12):
                for b in (0, 0, 63, 0, 63, 63, 0, 63):
                    t.out(7, b)
        geo_words(t, 0x5A, [8000, -6000, -12000])
    geo_words(t, 0x00, rot(0.5, 0.7, 0.2) + [0, 0, 1400])
    t.out(5, 0x48)
    t.out(7, 0x01 | (0x02 if faces else 0) | (0x04 if textured else 0))
    t.wait_ce()
    t.frame(2)
    t.shot("geo3d_" + ("textured" if textured else "faces" if faces else "wireframe"))
    return t


SCENARIOS = {
    "calibration": calibration,
    "calib_dots_256": lambda: calib_dots(False),
    "calib_dots_512": lambda: calib_dots(True),
    "calib_rows_256x212": calib_rows,
    "text1": text1,
    "text2": text2,
    "graphic1": graphic1,
    "graphic2_mag16": graphic2,
    "multicolor": multicolor,
    "graphic3_cc_ic": graphic3,
    "graphic4": lambda: bitmap_scene("G4", "graphic4"),
    "graphic4_tp": lambda: bitmap_scene("G4", "graphic4_tp", tp=True),
    "graphic5": lambda: bitmap_scene("G5", "graphic5"),
    "graphic6": lambda: bitmap_scene("G6", "graphic6"),
    "graphic7": lambda: bitmap_scene("G7", "graphic7"),
    "yjk": lambda: yjk_scene(False),
    "yae": lambda: yjk_scene(True),
    "hscroll_msk": lambda: hscroll(False),
    "hscroll_sp2": lambda: hscroll(True),
    "vscroll_r23": vscroll,
    "display_adjust": adjust,
    "v9968_epal": epal,
    "v9968_sp3": sp3_scene,
    "v9968_s16": s16_scene,
    "v9968_commands": commands_scene,
    "v9968_lrmm": lrmm_scene,
    "v9968_lfmm": lfmm_scene,
    "geo3d_wireframe": lambda: geo3d_scene(False, False),
    "geo3d_faces": lambda: geo3d_scene(True, False),
    "geo3d_textured": lambda: geo3d_scene(True, True),
}


def main():
    out = sys.argv[1]
    names = sys.argv[2:] or list(SCENARIOS)
    os.makedirs(out, exist_ok=True)
    for name in names:
        SCENARIOS[name]().save(os.path.join(out, name + ".txt"))
        print(name)


if __name__ == "__main__":
    main()

"""
Generate V9990 test scenarios as port traces (see host/replay.c for the
format). Each scenario is executed both in openMSX (GFX9000) and in our core,
and the screenshots are compared by compare.py.

    python3 scenarios.py <out_dir>
"""
import os
import sys


class Trace:
    def __init__(self):
        self.lines = []

    # --- raw port access -------------------------------------------------
    def out(self, port, value):
        self.lines.append(f"w 0x{0x60 | (port & 0x0F):02X} 0x{value & 0xFF:02X}")

    def inp(self, port):
        self.lines.append(f"r 0x{0x60 | (port & 0x0F):02X}")

    def frame(self, n=1):
        self.lines += ["frame"] * n

    def shot(self, name):
        self.lines.append(f"shot {name}")

    def wait_ce(self):
        self.lines.append("wait_ce")

    # --- V9990 helpers (what an MSX program would do) ----------------------
    def reg(self, r, value):
        self.out(4, r)
        self.out(3, value)

    def mode(self, r6, r7=0):
        """Set the display mode. openMSX applies R#6/R#7 at the next line
        (and the VRAM address mapping depends on the mode), so let a frame
        pass before touching VRAM."""
        self.reg(6, r6)
        self.reg(7, r7)
        self.frame()

    def regs(self, first, values):
        self.out(4, first)
        for v in values:
            self.out(3, v)

    def vram_addr(self, addr):
        self.regs(0, [addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0x07])

    def vram_write(self, addr, data):
        self.vram_addr(addr)
        for b in data:
            self.out(0, b)

    def palette(self, index, r, g, b):
        self.reg(14, (index << 2) & 0xFF)
        for c in (r, g, b):
            self.out(1, c)

    def lmmv(self, x, y, w, h, color, lop=0x0C):
        self.regs(36, [x & 0xFF, x >> 8, y & 0xFF, y >> 8, w & 0xFF, w >> 8, h & 0xFF, h >> 8,
                       0, lop, 0xFF, 0xFF, color & 0xFF, color >> 8])
        self.reg(52, 0x20)
        self.wait_ce()

    def lmmm(self, sx, sy, dx, dy, w, h, lop=0x0C, arg=0):
        self.regs(32, [sx & 0xFF, sx >> 8, sy & 0xFF, sy >> 8, dx & 0xFF, dx >> 8, dy & 0xFF, dy >> 8,
                       w & 0xFF, w >> 8, h & 0xFF, h >> 8, arg, lop, 0xFF, 0xFF])
        self.reg(52, 0x40)
        self.wait_ce()

    def save(self, path):
        with open(path, "w") as f:
            f.write("\n".join(self.lines) + "\n")


def reset(t):
    """Known state: soft reset, all VRAM cleared, palette 0 black."""
    t.out(7, 0x02)   # SRS soft reset
    t.out(7, 0x00)
    t.reg(9, 0)      # no interrupts
    t.reg(8, 0)      # display off while clearing
    # 512KB = 1024 x 256 pixels at 16bpp
    t.mode(0x80 | (2 << 2) | 3)
    t.lmmv(0, 0, 1024, 256, 0x0000)
    for i in range(64):
        t.palette(i, 0, 0, 0)


def grb555(r, g, b):
    return (g << 10) | (r << 5) | b


# ---------------------------------------------------------------------------

def calibration():
    """P1 full of one colour: finds the display area inside openMSX's frame."""
    t = Trace()
    reset(t)
    t.mode(0x05)
    t.reg(8, 0x80 | 0x40)               # display on, sprites off
    t.palette(0, 0, 0, 0)
    t.palette(1, 31, 31, 31)
    t.reg(15, 0)
    t.vram_write(0x00000, [0x11] * 4)  # pattern 0, line 0..7: colour 1
    for y in range(1, 8):
        t.vram_write(y * 128, [0x11] * 4)
    t.frame(2)
    t.shot("calibration")
    return t


def calib_dots(p2):
    """Alternating white/black dots on every line: gives the exact position
    of each V9990 dot in openMSX's horizontally stretched picture."""
    t = Trace()
    reset(t)
    t.mode(0x59 if p2 else 0x05)
    t.reg(8, 0x80 | 0x40)
    t.reg(13, 0)
    t.palette(1, 31, 31, 31)
    pitch = 256 if p2 else 128
    for y in range(8):
        t.vram_write(y * pitch, [0x10] * 4)     # pattern 0: 1,0,1,0,...
    t.frame(2)
    t.shot("calib_dots_512" if p2 else "calib_dots_256")
    return t


def p1_scene():
    t = Trace()
    reset(t)
    t.mode(0x05)
    t.reg(8, 0x80)
    t.reg(13, 0x00 | (0x01 << 2))
    for i in range(1, 16):
        t.palette(i, i * 2, 31 - i * 2, 20)
        t.palette(16 + i, 4, i, 10)
        t.palette(48 + i, 31, 31, i * 2)
    t.palette(0, 0, 0, 6)
    for y in range(8):
        frame = [0xFF] * 4 if y in (0, 7) else [0xF0, 0, 0, 0x0F]
        t.vram_write(0x00000 + y * 128 + 4, frame)
        t.vram_write(0x00000 + y * 128 + 8, [0x88] * 4)
        t.vram_write(0x40000 + y * 128 + 4, [0x33] * 4)
        t.vram_write(0x40000 + y * 128 + 8, [0x99] * 4)
    for ty in range(64):
        rowb, rowa = [], []
        for tx in range(64):
            rowb += [1 if (tx ^ ty) & 1 else 2, 0]
            pa = 0
            if 6 <= tx < 22 and 6 <= ty < 18:
                pa = 1 if tx in (6, 21) or ty in (6, 17) else (0 if (tx + ty) % 5 else 2)
            rowa += [pa, 0]
        t.vram_write(0x7E000 + ty * 128, rowb)
        t.vram_write(0x7C000 + ty * 128, rowa)
    t.reg(19, 4)
    t.reg(25, 0x02)                     # sprite generator at 0x08000
    for y in range(16):
        row = []
        for x in range(0, 16, 2):
            def px(xx):
                dx, dy = xx - 8, y - 8
                return 1 + (y + xx) % 15 if dx * dx + dy * dy < 56 else 0
            row.append((px(x) << 4) | px(x + 1))
        t.vram_write(0x08000 + y * 128, row)
    spat = []
    for n in range(125):
        on = n < 12
        spat += [70 + (n % 3) * 20 if on else 230, 0, (30 + n * 18) & 0xFF,
                 0xC0 | (0x20 if n & 1 else 0) | (0 if on else 0x10)]
    t.vram_write(0x3FE00, spat)
    t.frame(2)
    t.shot("p1_scene")
    # scroll layer B and A, move sprites
    t.reg(23, 3)
    t.reg(24, 2)
    t.reg(17, 12)
    t.frame(2)
    t.shot("p1_scroll")
    return t


def p2_scene():
    t = Trace()
    reset(t)
    t.mode(0x59)
    t.reg(8, 0x80)
    t.reg(13, 0x00 | (0x01 << 2))
    for i in range(1, 16):
        t.palette(i, 31, i * 2, 0)
        t.palette(16 + i, 0, i * 2, 31)
        t.palette(48 + i, 31, 31, 31 - i)
    for y in range(8):                  # pattern 1: stripes, both palettes
        t.vram_write(y * 256 + 4, [(y + 1) * 0x11 & 0xFF] * 4)
    for ty in range(64):
        t.vram_write(0x7C000 + ty * 256, sum(([1 if (tx + ty) % 3 else 0, 0] for tx in range(128)), []))
    t.reg(25, 0x01)                     # sprite generator at 0x08000
    for y in range(16):
        t.vram_write(0x08000 + y * 256, [0x55 if 4 <= y < 12 else 0] * 8)
    spat = []
    for n in range(125):
        on = n < 10
        spat += [60 + n * 8 if on else 230, 0, (40 + n * 40) & 0xFF, 0xC0 | (((40 + n * 40) >> 8) & 3) | (0 if on else 0x10)]
    t.vram_write(0x7BE00, spat)
    t.frame(2)
    t.shot("p2_scene")
    return t


def bitmap_bd16():
    t = Trace()
    reset(t)
    t.mode(0x80 | 3)                  # B1, 256 wide, 16bpp
    t.reg(8, 0x80)
    t.palette(0, 4, 4, 8)
    for y in range(212):
        row = []
        for x in range(256):
            c = grb555(x >> 3, y * 31 // 211, 31 - (x >> 3))
            row += [c & 0xFF, c >> 8]
        t.vram_write(y * 512, row)
    t.lmmv(20, 20, 60, 40, grb555(31, 31, 31))
    t.lmmv(100, 80, 80, 60, grb555(31, 0, 0))
    t.lmmm(20, 20, 160, 150, 60, 40)
    t.frame(2)
    t.shot("bitmap_bd16")
    return t


def bitmap_bp4_b3():
    t = Trace()
    reset(t)
    t.mode(0x80 | 0x10 | (1 << 2) | 1)   # B3: 512 wide display, 512 image, 4bpp
    t.reg(8, 0x80)
    t.reg(13, 0x04)                     # palette offset 16
    for i in range(16):
        t.palette(16 + i, i * 2, 31 - i * 2, (i * 5) % 32)
    for y in range(212):
        t.vram_write(y * 256, [(((x * 2 + y // 8) & 15) << 4) | ((x * 2 + 1 + y // 8) & 15) for x in range(256)])
    t.frame(2)
    t.shot("bitmap_bp4_b3")
    return t


SCENARIOS = {
    "calibration": calibration,
    "calib_dots_256": lambda: calib_dots(False),
    "calib_dots_512": lambda: calib_dots(True),
    "p1_scene": p1_scene,
    "p2_scene": p2_scene,
    "bitmap_bd16": bitmap_bd16,
    "bitmap_bp4_b3": bitmap_bp4_b3,
}

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn in SCENARIOS.items():
        fn().save(os.path.join(out, name + ".txt"))
        print("wrote", name)

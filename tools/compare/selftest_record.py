"""
Self-test of the recording path: turn a small V9990 scene into a Z80
program (LD A,n / OUT (n),A), which openMSX runs from RAM while record.tcl
records the real CPU port accesses.

    python3 selftest_record.py <program.bin> <loader.tcl>
"""
import sys

from scenarios import Trace, reset

ORG = 0x8000                 # page 2 RAM (page 3 holds the stack and C-BIOS work area)


def scene():
    t = Trace()
    reset(t)
    t.mode(0x05)                         # P1
    t.reg(8, 0x80)
    t.reg(13, 0x00 | (0x01 << 2))
    for i in range(1, 16):
        t.palette(i, 31, i * 2, 4)
        t.palette(16 + i, 2, i, 16)
        t.palette(48 + i, 31, 31, i)
    for y in range(8):
        t.vram_write(0x00000 + y * 128 + 4, [0x12, 0x34, 0x56, 0x78])
        t.vram_write(0x40000 + y * 128 + 4, [0x99] * 4 if y % 2 else [0x00] * 4)
    for ty in range(27):
        t.vram_write(0x7E000 + ty * 128, [1, 0] * 32)
        t.vram_write(0x7C000 + ty * 128 + 8, [1, 0] * (4 + ty % 5))
    t.reg(25, 0x02)
    for y in range(16):
        t.vram_write(0x08000 + y * 128, [0x77 if 3 < y < 12 else 0x00] * 8)
    spat = []
    for n in range(125):
        on = n < 6
        spat += [40 + n * 20 if on else 230, 0, 20 + n * 35, 0xC0 | (0 if on else 0x10)]
    t.vram_write(0x3FE00, spat)
    return t


def assemble(trace):
    code = bytearray([0xF3])                         # DI
    a = None                                         # known value of register A
    for line in trace.lines:
        parts = line.split()
        if parts[0] == "w":
            port, val = int(parts[1], 0), int(parts[2], 0)
            if a != val:
                code += bytes([0x3E, val])           # LD A,val
                a = val
            code += bytes([0xD3, port])              # OUT (port),A
        elif parts[0] == "wait_ce":
            code += bytes([0xDB, 0x65,               # IN A,(65h)
                           0xE6, 0x01,               # AND 1
                           0x20, 0xFA])              # JR NZ,-6
            a = None
        # "frame" / "shot" lines have no CPU equivalent here
    code += bytes([0x18, 0xFE])                      # JR $
    assert len(code) < 0x6000, len(code)
    return bytes(code)


if __name__ == "__main__":
    prog = assemble(scene())
    with open(sys.argv[1], "wb") as f:
        f.write(prog)
    with open(sys.argv[2], "w") as f:
        f.write(f"""# load and start the self-test program once C-BIOS has booted
after time 2 {{
    if {{[catch {{
        set fd [open {{{sys.argv[1]}}} rb]
        fconfigure $fd -translation binary
        debug write_block memory 0x{ORG:04X} [read $fd]
        close $fd
        reg pc 0x{ORG:04X}
        puts stderr "self-test program started ({len(prog)} bytes)"
    }} err]}} {{ puts stderr "loader ERROR: $err" }}
}}
""")
    print(f"{len(prog)} bytes")

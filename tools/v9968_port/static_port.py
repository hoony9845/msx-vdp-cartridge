#!/usr/bin/env python3
"""
Internal V9968 (98h-9Ch, Geo3D 9Dh/9Fh) ROM -> V9968 cartridge (88h-8Ch,
8Dh/8Fh) ROM, from the bytes alone (no emulator).

Every byte pair that could be an instruction using one of those ports is a
candidate: OUT (n),A / IN A,(n), and LD C,n / LD BC,nn (C then used by
OUT (C)/OTIR/OUTI/...). A candidate is changed when the bytes around it read
as Z80 code:
  - the instructions before it (back to the start of its routine: after a
    jump, a return or padding) and after it (up to the next jump or return,
    following JR) are scored with opcode statistics of real code against
    data (z80_code_model.py), averaged per instruction;
  - LD C / LD BC needs a user of C close behind: OUT (C)-type, or a JP/CALL
    to a routine it hands the port to;
  - runs of one byte (graphics) disqualify.

  static_port.py <internal.rom> -o <external.rom> [-v] [--mute-internal-vdp]

Measured on 39 V9968 / Geo3D ROMs with known conversions (scored with the
statistics of the other family of ROMs): 2050 of 2052 port operands found,
3 data bytes changed by mistake. Check the result in an emulator or on the
machine; -v lists every candidate with its score so doubtful ones can be
looked at, and --force/--skip correct single offsets.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from z80len import length
from z80_code_model import W, UNKNOWN

PORTS = set(range(0x98, 0x9E)) | {0x9F}
ED_C_IO = {0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x78, 0x41, 0x49, 0x51, 0x59, 0x61, 0x69, 0x79,
           0xA2, 0xA3, 0xAA, 0xAB, 0xB2, 0xB3, 0xBA, 0xBB}
STOPS = (0xC3, 0xC9, 0x18, 0xE9)
THRESHOLD = 0.8


def key(m, o):
    b = m[o]
    if b in (0xED, 0xCB, 0xDD, 0xFD) and o + 1 < len(m):
        c = m[o + 1]
        return 0x100 + (c >> 6) if b == 0xCB else (b << 8) | c
    return b


def jr_target(m, o):
    d = m[o + 1]
    return o + 2 + (d - 256 if d > 127 else d)


def context(m, p):
    """Instruction offsets around the candidate at p: its routine before it
    and its straight-line code after it."""
    o, back = max(p - 40, 0), []
    while o < p:
        n = length(m, o)
        if n is None:
            break
        back.append(o)
        o += n
    back = back[-10:] if o == p else []
    for i in range(len(back) - 1, -1, -1):
        x = back[i]
        if m[x] in STOPS or (m[x] in (0x00, 0xFF) and x + 1 < len(m) and m[x + 1] == m[x]):
            back = back[i + 1:]
            break
    fwd, o, jumps = [], p, 0
    while len(fwd) < 12 and o < len(m):
        n = length(m, o)
        if n is None:
            break
        fwd.append(o)
        if m[o] == 0x18 and jumps < 2 and o + 1 < len(m) and m[o + 1] < 0x80:
            o = jr_target(m, o)
            jumps += 1
            continue
        if m[o] in STOPS:
            break
        o += n
    return back, fwd


def c_user(m, p):
    """True / 'jp' (hands the port to a routine) / False for LD C / LD BC."""
    o = p + (2 if m[p] == 0x0E else 3)
    for _ in range(8):
        k = length(m, o)
        if k is None:
            return False
        if m[o] == 0xED and m[o + 1] in ED_C_IO:
            return True
        if m[o] == 0xED and m[o + 1] in (0xB0, 0xB8, 0xB1, 0xB9, 0xA0, 0xA8, 0xA1, 0xA9):
            return False                        # LDIR & co.: BC is a byte count
        if m[o] in (0xCD, 0xC3):
            return 'jp' if o + 2 < len(m) and m[o + 2] >= 0x40 else False
        if m[o] == 0x18 and o + 1 < len(m):
            o = jr_target(m, o)
            continue
        if m[o] == 0xC9:
            return False
        o += k
    return False


def longest_run(m, p):
    """Longest run of one byte near p that cannot be repeated code: a run
    of INC HL or RRCA is code, a run of FEh (CP n taking the next FEh as its
    operand) is not."""
    best = c = 1
    for i in range(max(p - 9, 1), min(p + 11, len(m))):
        c = c + 1 if m[i] == m[i - 1] and m[i] not in (0x00, 0xFF) and length(m, i) != 1 else 1
        best = max(best, c)
    return best


def in_step(m, p):
    """Share of decodings started 8-40 bytes before p that land on p (not
    inside an instruction covering it)."""
    hit = n = 0
    for s in range(max(p - 40, 0), max(p - 8, 0)):
        o = s
        while o < p:
            k = length(m, o)
            if k is None:
                break
            o += k
        n += 1
        hit += o == p
    return hit / n if n else 1.0


def vdp_io(m, x):
    return (m[x] in (0xD3, 0xDB) and x + 1 < len(m) and m[x + 1] in PORTS) or \
        (m[x] == 0xED and x + 1 < len(m) and m[x + 1] in ED_C_IO)


def io_near(m, p, reach=96):
    """Direct VDP I/O on an instruction chain near p (a LD C,port handed
    to a routine should have its routine's kind of code around)."""
    for s in (max(p - reach, 0), p):
        o = s
        while o < min(p + reach, len(m)):
            k = length(m, o)
            if k is None:
                break
            if o != p and vdp_io(m, o):
                return True
            o += k
    return False


def score(m, p):
    back, fwd = context(m, p)
    ins = back + fwd[1:]
    s = sum(W.get(key(m, x), UNKNOWN) for x in ins) / max(len(ins), 1)
    # other VDP I/O on the same chain of instructions: VDP code
    s += 0.5 * min(2, sum(1 for x in ins if vdp_io(m, x)))
    if in_step(m, p) < 0.3:
        s -= 3                                  # most likely inside another instruction
    cu = c_user(m, p) if m[p] in (0x0E, 0x01) else None
    if cu == 'jp' and m[p] == 0x01 and m[p + 2] == 0xFF:
        cu = False                              # LD BC,-100 (FF9Ch): a number, not a port
    if cu is False:
        s -= 10                                 # C never used as a port
    elif cu == 'jp' and not io_near(m, p):
        s -= 2                                  # handed to a routine far from any VDP I/O
    elif cu:
        s += 0.2
    if len(ins) < 2 and cu != 'jp':
        s -= 5
    if longest_run(m, p) >= 4:
        s -= 5
    return s


def candidates(rom):
    return [p for p in range(len(rom) - 2) if rom[p] in (0xD3, 0xDB, 0x0E, 0x01) and rom[p + 1] in PORTS]


def find_sites(rom, romtype=None):
    return [p + 1 for p in candidates(rom) if score(rom, p) >= THRESHOLD]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('-v', '--verbose', action='store_true', help='list every candidate with its score')
    ap.add_argument('--force', default='', help='ROM offsets (hex, comma separated) to change as well')
    ap.add_argument('--skip', default='', help='ROM offsets (hex, comma separated) to leave alone')
    ap.add_argument('--mute-internal-vdp', action='store_true',
                    help="turn the built-in VDP's screen and interrupts off at start (demos that run "
                         "on the V9968's interrupts and stay black otherwise)")
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    out = bytearray(rom)
    sites = set(find_sites(rom))
    sites |= {int(x, 16) for x in a.force.split(',') if x.strip()}
    sites -= {int(x, 16) for x in a.skip.split(',') if x.strip()}
    for o in sites:
        if rom[o] not in PORTS:
            sys.exit('%X: %02X is not a 98h-9Fh port' % (o, rom[o]))
        out[o] = rom[o] - 0x10
    if a.mute_internal_vdp:
        from v9968_port import mute_internal_vdp
        print('mute built-in VDP: ' + mute_internal_vdp(out))
    open(a.out, 'wb').write(out)
    cands = candidates(rom)
    close = [p for p in cands if p + 1 not in sites and THRESHOLD - 0.5 <= score(rom, p) < THRESHOLD]
    print('%s: %d port operands changed; %d port-like bytes left as data (%d of them close to the line, see -v)'
          % (a.out, len(sites), len(cands) - len(sites), len(close)))
    if a.verbose:
        names = {0xD3: 'OUT (n),A', 0xDB: 'IN A,(n) ', 0x0E: 'LD C,n   ', 0x01: 'LD BC,nn '}
        for p in cands:
            sc = score(rom, p)
            if sc < THRESHOLD - 1.5 and p + 1 not in sites:
                continue
            print('  %s %06X %s %02X  score %5.2f  %s' % ('CHANGE' if p + 1 in sites else 'leave ', p + 1,
                  names[rom[p]], rom[p + 1], sc, rom[max(p - 8, 0):p + 8].hex(' ')))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Convert a ROM made for an internal V9968 (I/O 98h-9Ch, geo3d 9Dh/9Fh) into
one for the external cartridge (88h-8Ch, 8Dh/8Fh), from a trace of the
port accesses the software really makes (trace.sh, openMSX V9968 fork).

Only instructions that ran are changed, so data that happens to look like
an OUT instruction stays as it is:
  OUT (n),A / IN A,(n)                       the operand n
  OUT (C),r / IN r,(C) / OUTI / OTIR / ...   the LD C,n or LD BC,nn before it
                                             (nearest one with that port)
Accesses made by the BIOS (not in the ROM) are left alone.

  v9968_port.py <internal.rom> <trace.txt> [more traces...] -o <external.rom>
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from z80len import length, STOP

PORTS = set(range(0x98, 0x9D)) | {0x9D, 0x9F}
ED_C_IO = {0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x78,          # IN r,(C)
           0x41, 0x49, 0x51, 0x59, 0x61, 0x69, 0x79,          # OUT (C),r
           0xA2, 0xA3, 0xAA, 0xAB, 0xB2, 0xB3, 0xBA, 0xBB}    # INI OUTI IND OUTD and repeats
BACK = 40                                                   # bytes before the PC in a record
PAST_STOPS = int(os.environ.get('V9968_PORT_PAST_STOPS', '64'))   # follow on past returns/jumps
MAX_STEPS = int(os.environ.get('V9968_PORT_MAX_STEPS', '4000'))


def load_trace(path):
    for line in open(path):
        f = line.split()
        if len(f) != 5:
            continue
        yield dict(pc=int(f[0], 16), port=int(f[1], 16), kind=f[2], bc=int(f[3]), mem=bytes.fromhex(f[4]))


CBIOS_DIRS = [os.path.expanduser('~/tools/openmsx16/root/usr/share/openmsx/machines/C-BIOS_MSX2+/roms'),
              os.path.expanduser('~/tools/openmsx16/root/usr/share/openmsx/systemroms')]


def bios_images():
    """The C-BIOS ROMs of the tracing machine: its logo and sub ROM run code
    in page 2 too, which is not the software's."""
    out = []
    for d in CBIOS_DIRS:
        if os.path.isdir(d):
            for n in sorted(os.listdir(d)):
                if n.startswith('cbios') and n.endswith('.rom'):
                    out.append(open(os.path.join(d, n), 'rb').read())
    return out


def find_all(rom, pat):
    out, i = [], rom.find(pat)
    while i >= 0:
        out.append(i)
        i = rom.find(pat, i + 1)
    return out


def locate(rom, mem):
    """ROM offsets of the record's PC: the longest stretch around it found."""
    for lo, hi in ((0, len(mem)), (BACK - 24, len(mem)), (BACK - 12, BACK + 4), (BACK - 6, BACK + 2)):
        hits = find_all(rom, mem[lo:hi])
        if hits:
            return [h + (BACK - lo) for h in hits], hi - lo
    return [], 0


def io_instruction(mem):
    """(kind, offset of the instruction from the PC) of the access. In the
    watchpoint the PC is at the instruction's opcode: its first byte, or
    the second of an ED-prefixed one (a repeating one may be back on ED)."""
    if mem[BACK] in (0xD3, 0xDB):
        return 'imm', 0
    if mem[BACK - 1] == 0xED and mem[BACK] in ED_C_IO:
        return 'c', -1
    if mem[BACK] == 0xED and mem[BACK + 1] in ED_C_IO:
        return 'c', 0
    if mem[BACK - 2] in (0xD3, 0xDB):                    # PC already past it
        return 'imm', -2
    if mem[BACK - 2] == 0xED and mem[BACK - 1] in ED_C_IO:
        return 'c', -2
    return None, 0


def find_c_setter(rom, at, port, reach=512):
    """Nearest LD C,port / LD BC,(xx)port before offset `at`, looking past
    an unrolled run of OUTI/OUTD/INI/IND first."""
    while at >= 2 and rom[at - 2] == 0xED and rom[at - 1] in (0xA2, 0xA3, 0xAA, 0xAB):
        at -= 2
    for o in range(at - 2, max(at - reach, 0) - 1, -1):
        if rom[o] == 0x0E and rom[o + 1] == port:
            return o + 1
        if rom[o] == 0x01 and rom[o + 1] == port:
            return o + 1
    return None


def mute_internal_vdp(out):
    """Run a few instructions before the ROM's own start that turn the
    built-in VDP's screen and interrupts off (R#0 = R#1 = 0 through 99h), as
    the official external builds of interrupt-driven demos do: otherwise the
    built-in VDP's VBLANK interrupts mix with the cartridge's. The code goes
    into a free (00h/FFh) stretch of the first 8KB, the ROM header's INIT
    is pointed at it and it jumps on to the old INIT."""
    if out[0:2] != b'AB':
        return 'no AB header'
    init = out[2] | out[3] << 8
    if not 0x4000 <= init < 0x6000:
        return 'INIT %04X is not in the first 8KB' % init
    stub = bytes([0xF3, 0xAF, 0xD3, 0x99, 0x3E, 0x80, 0xD3, 0x99,      # DI; R#0 = 0
                  0xAF, 0xD3, 0x99, 0x3E, 0x81, 0xD3, 0x99,            # R#1 = 0
                  0xC3, init & 0xFF, init >> 8])                       # JP INIT
    for fill in (0xFF, 0x00):
        o = bytes(out[0:0x2000]).find(bytes([fill]) * (len(stub) + 4), 0x10)
        if o >= 0:
            o += 2
            out[o:o + len(stub)] = stub
            out[2], out[3] = (0x4000 + o) & 0xFF, (0x4000 + o) >> 8
            return 'start code at %04X, then INIT %04X' % (0x4000 + o, init)
    return 'no free space in the first 8KB'


A_LOADS = {0x3E, 0xAF, 0x7E, 0x1A, 0x0A, 0xF1, 0x3A} | set(range(0x78, 0x80))


def looks_like_code(rom, o, code_pages):
    """Is the port-operand-like byte at `o` (its instruction at o-1) likely
    to be a real instruction that did not run? It must be in an 8KB page
    with code that ran, and look like VDP code: OUT/IN next to a load of A
    or another OUT/IN, or LD C/LD BC followed shortly by OUT (C)/OTIR/...
    or a CALL."""
    if o // 0x2000 not in code_pages:
        return False
    op = rom[o - 1]
    if op in (0xD3, 0xDB):
        prev = rom[o - 3:o - 1]
        nxt = rom[o + 1:o + 3]
        return (len(prev) == 2 and (prev[1] in A_LOADS or prev[0] in (0x3E, 0xD3, 0xDB)) or
                len(nxt) == 2 and nxt[0] in (0xD3, 0xDB, 0x3E, 0xAF, 0xC9))
    if op in (0x0E, 0x01):
        a = o + (1 if op == 0x0E else 2)
        for k in range(a, min(a + 10, len(rom) - 1)):
            if rom[k] == 0xED and rom[k + 1] in ED_C_IO or rom[k] == 0xCD:
                return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('traces', nargs='+')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('-q', '--quiet', action='store_true')
    ap.add_argument('--mute-internal-vdp', action='store_true',
                    help="turn the built-in VDP's screen and interrupts off at start "
                         "(needed by demos that run on the V9968's interrupts)")
    ap.add_argument('--candidates', action='store_true',
                    help='list the port-like bytes in code pages that were not seen running')
    ap.add_argument('--force', default='', help='ROM offsets (hex, comma separated) to change as well')
    ap.add_argument('--skip', default='', help='ROM offsets (hex, comma separated) to leave alone')
    ap.add_argument('--take-likely', action='store_true',
                    help='also change every candidate that looks like code (risky: may hit data)')
    args = ap.parse_args()
    force = [int(x, 16) for x in args.force.split(',') if x.strip()]
    skip = {int(x, 16) for x in args.skip.split(',') if x.strip()}

    rom = open(args.rom, 'rb').read()
    out = bytearray(rom)
    patched = {}            # offset -> description
    unresolved, bios = [], 0
    hidden = set()
    late = []               # OUT (C)-type without a setter near: settled at the end
    bioses = bios_images()
    seen = set()
    anchors = set()
    for path in args.traces:
        for r in load_trace(path):
            key = (r['pc'], r['port'], r['kind'], r['mem'])
            if key in seen:
                continue
            seen.add(key)
            if r['port'] not in PORTS:
                continue
            kind, rel = io_instruction(r['mem'])
            offs, matched = locate(rom, r['mem'])
            if not offs:
                if r['pc'] < 0x4000 or any(locate(b, r['mem'])[0] for b in bioses):
                    bios += 1       # the BIOS (page 0, or C-BIOS's logo/sub ROM code)
                else:               # code that is not in the ROM image as it runs
                    hidden.add('%04X %s port %02X' % (r['pc'], r['kind'], r['port']))
                continue
            for pc_off in offs:
                ins = pc_off + rel
                anchors.add(ins)
                if kind == 'imm' and rom[ins + 1] == r['port']:
                    patched[ins + 1] = '%04X %s (n) port %02X' % (r['pc'], 'OUT' if rom[ins] == 0xD3 else 'IN', r['port'])
                elif kind == 'c':
                    s = find_c_setter(rom, ins, r['port'])
                    if s is None:
                        late.append((ins, r['port'], '%04X: %s via C=%02X, no LD C/LD BC with it before ROM offset %X'
                                     % (r['pc'], r['kind'], r['port'], ins)))
                    else:
                        patched[s] = '%04X %s via C: %s at %X' % (
                            r['pc'], r['kind'], 'LD C,n' if rom[s - 1] == 0x0E else 'LD BC,nn', s - 1)
                else:
                    unresolved.append('%04X: %s port %02X, instruction not recognised' % (r['pc'], r['kind'], r['port']))
    # Code that did not run within the trace (a rarely taken path, a setter
    # in the caller of the OUT): follow the instructions forwards from each
    # instruction that ran, as long as they decode, up to the next
    # unconditional jump or return, and take the same operand forms there,
    # plus LD C,n / LD BC,nn that an OUT (C)-type instruction or a CALL
    # follows within a few instructions (n a port seen through C).
    c_ports = set()
    for path in args.traces:
        for r in load_trace(path):
            if r['port'] in PORTS and io_instruction(r['mem'])[0] == 'c':
                c_ports.add(r['port'])
    swept = 0
    for start in sorted(anchors):
        o, steps, stops = start, 0, 0
        recent = []                     # recent instructions (offset, length)
        while steps < MAX_STEPS:
            n = length(rom, o)
            if n is None:
                break
            b = rom[o]
            if b in (0xD3, 0xDB) and rom[o + 1] in PORTS and (o + 1) not in patched:
                patched[o + 1] = 'static: %s (n) port %02X' % ('OUT' if b == 0xD3 else 'IN', rom[o + 1])
                swept += 1
            is_cio = b == 0xED and rom[o + 1] in ED_C_IO
            if is_cio or b == 0xCD:
                for po, pn in recent[-4:]:
                    pb = rom[po]
                    if pb in (0x0E, 0x01) and rom[po + 1] in PORTS and (po + 1) not in patched and \
                            (is_cio or rom[po + 1] in c_ports):
                        patched[po + 1] = 'static: %s port %02X before %s' % (
                            'LD C,n' if pb == 0x0E else 'LD BC,nn', rom[po + 1], 'OUT/IN (C)' if is_cio else 'CALL')
                        swept += 1
            recent.append((o, n))
            if b in STOP or (b == 0xED and rom[o + 1] in (0x45, 0x4D)):
                stops += 1
                if stops > PAST_STOPS:
                    break
                recent = []
            o += n
            steps += 1
    # A routine entered with C already set (a jump in from code that set it,
    # often through an unrolled OUTI run): fine if a setter of that port
    # further before it is changed.
    for ins, port, msg in late:
        if not any(ins - 4096 <= o < ins and rom[o] == port for o in patched):
            unresolved.append(msg)
    code_pages = {a // 0x2000 for a in anchors}
    likely = sorted(i + 1 for i in range(1, len(rom) - 1)
                    if rom[i] in (0xD3, 0xDB, 0x0E, 0x01) and rom[i + 1] in PORTS and i + 1 not in patched
                    and looks_like_code(rom, i + 1, code_pages))
    for o in force:
        if rom[o] in PORTS:
            patched[o] = 'forced'
        else:
            print('  --force %X: %02X is not a 98h-9Fh port' % (o, rom[o]))
    if args.take_likely:
        for o in likely:
            patched.setdefault(o, 'likely code, not seen running')
    for o in skip:
        patched.pop(o, None)
    for o in patched:
        out[o] = rom[o] - 0x10
    if args.mute_internal_vdp:
        print('  mute built-in VDP: ' + mute_internal_vdp(out))
    open(args.out, 'wb').write(out)

    # what a plain search would also have changed: left alone, listed for a look
    others = [i + 1 for i in range(len(rom) - 1)
              if rom[i] in (0xD3, 0xDB, 0x0E, 0x01) and rom[i + 1] in PORTS and i + 1 not in patched]
    print('  (%d of them found by following the code from what ran)' % swept)
    print('%s: %d port operands changed, %d accesses by the BIOS ignored, %d unresolved, '
          '%d other 9xh operand-like bytes left (not seen running)' %
          (args.out, len(patched), bios, len(set(unresolved)), len(others)))
    if not args.quiet:
        for o in sorted(patched):
            print('  changed %06X %02X->%02X  %s' % (o, rom[o], out[o], patched[o]))
    left = [o for o in likely if o not in patched]
    if left:
        print('  %d of the bytes left look like code that did not run (play further, or see --candidates)'
              % len(left))
    if args.candidates:
        for o in left:
            print('  candidate %06X: %s [%02X] %s' % (o, rom[max(o - 9, 0):o - 1].hex(' '), rom[o],
                                                   rom[o + 1:o + 9].hex(' ')))
    for u in sorted(set(unresolved)):
        print('  UNRESOLVED', u)
    if hidden:
        print('  NOT CONVERTIBLE: %d port accesses run from RAM code that is not in the ROM image '
              '(compressed or generated at run time), e.g. %s' % (len(hidden), ', '.join(sorted(hidden)[:4])))
    return 1 if unresolved or hidden else 0


if __name__ == '__main__':
    sys.exit(main())

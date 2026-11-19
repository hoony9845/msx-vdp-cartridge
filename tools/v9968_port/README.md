# Internal V9968 ROM -> V9968 cartridge ROM

Converts software built for an MSX with a built-in V9968 (I/O 98h-9Ch,
Geo3D 9Dh/9Fh) to run with the V9968 cartridge (88h-8Ch, 8Dh/8Fh).

## Quick: from the bytes alone

    tools/v9968_port/static_port.py <internal.rom> -o <external.rom> [-v]

This takes under a second and needs no emulator. Every byte pair that could
be a port instruction (`OUT (n),A`, `IN A,(n)`, or `LD C,n` / `LD BC,nn`
followed by `OUT (C)`/`OTIR`/... or a handoff to a routine) is checked:

- The code around it is scored with opcode statistics of real Z80 code
  against data (`z80_code_model.py`).
- The candidate must be in step with the instructions before it, not inside
  one of them.
- Other VDP I/O on the same instruction chain counts in its favour.
- Runs of one byte (graphics) count against it.
- `LD BC,FF9Ch` (-100) and `LD BC,n` + `LDIR` (a byte count) are rejected.

Results:
- 39 V9968/Geo3D ROMs with known conversions: 2052 of 2052 port operands
  found, 0 data bytes changed. With the statistics of the other family of
  ROMs (cross-checked): 2050 found, 3 wrong.
- 7 other MSX games by other authors, against the emulator traces: no wrong
  change. It also found 49 real VDP sites that the 5-minute traces never ran,
  and missed 1 (an interrupt handler).

`-v` lists the candidates with their scores. `--force` and `--skip` correct
single offsets. `--mute-internal-vdp` handles demos that stay black (see
below). Check the result in an emulator or on the machine. If in doubt, use
the traced conversion below.

## Traced: run it in the emulator

    tools/v9968_port/convert.sh <internal.rom> <ASCII16|ASCII8> [output.rom]

ASCII16 for the Geo3D demos, ASCII8 for the V9968 sample demos. Needs the
openMSX V9968 fork (`tools/compare/v9968/openmsx_env.sh`). One ROM takes
1-2 minutes:

1. `trace.sh` runs the ROM for 5 minutes of emulated time on an MSX2+ with
   a built-in V9968 and notes every instruction that touches 98h-9Fh
   (`trace_io.tcl`).
2. `v9968_port.py` lowers the port operand of those instructions by 10h:
   `OUT (n),A` / `IN A,(n)`, and for `OUT (C)`/`OTIR`/`OUTI`/... the
   `LD C,n` / `LD BC,nn` that set C. Code that did not run in the 5 minutes
   is found by following the instructions on from the code that did.
   Bytes that only look like such instructions (data) are left alone.
   Accesses by the BIOS are ignored.
3. `verify.sh` runs the original on the built-in-V9968 machine and the
   result on an MSX2+ with the cartridge, and checks from screenshots that
   the cartridge picture is not black or frozen (`compare_shots.py`).
   If it is, the ROM is converted again with `--mute-internal-vdp`: a few
   instructions before the ROM's start turn the built-in VDP's screen and
   interrupts off (R#0 = R#1 = 0), as the official external builds of the
   interrupt-driven demos do.

Checked against the official external builds: the 12 Geo3D-V2 demos and
CAPE CIRCUIT, PRISM FLIGHT, SUPER CAT and LUMEN FORGE come out byte-identical;
MIST VALE and CATSTRIDER (official builds rebuilt, not just re-ported) run
like the original with the built-in VDP muted.

## Games

A game does not play itself through, so code for later stages, menus or the
ending may not run in the 5-minute trace. Two things cover that:

- Following the code from what ran (step 2) already finds most of it.
- Play it yourself: `tools/v9968_port/play.sh <internal.rom> <romtype>` opens
  an openMSX window (MSX2+ with the built-in V9968) and traces while you play.
  Close the window when done. Each session leaves a trace in `<rom>.traces/`,
  and `convert.sh` uses them all. Play several times to cover more.

After converting, `v9968_port.py` says how many port-like bytes still look
like code that never ran. `--candidates` lists them. If you have checked them,
`PORT_OPTS="--take-likely"` changes them all, or `PORT_OPTS="--force 1A2B,..."`
changes only some (`--skip` leaves an offset alone). On the reference ROMs,
`--take-likely` changed no data byte. With a short trace and no code
following, it found 49 of the 53 missed sites.

Limits: the tracing machine has C-BIOS only, with no BASIC, disk or Kanji ROM.
Code that is unpacked into RAM and run there cannot be converted by changing
operands; the converter reports it as NOT CONVERTIBLE.

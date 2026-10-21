"""
Generate the KiCad 8 schematic (+ project symbol library) from design.py.

Every used pin gets a short wire stub ending in a net label (or a power
symbol for power nets); unused pins get a no-connect flag.

    python3 gen_schematic.py <project_dir>
"""
import math
import os
import sys
import uuid

import design
from sexp import find_all, find_block, parse, unquote

KICAD_SYMBOLS = "/usr/share/kicad/symbols"
GRID = 2.54
STUB = 2.54

out_dir = sys.argv[1]
sch_path = os.path.join(out_dir, design.PROJECT + ".kicad_sch")
root_uuid = str(uuid.uuid4())


def uid():
    return str(uuid.uuid4())


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def fnum(v):
    v = round(v, 4)
    return ("%.4f" % v).rstrip("0").rstrip(".") if v != int(v) else str(int(v))


# ---------------------------------------------------------------------------
# Custom symbol generation
# ---------------------------------------------------------------------------

def custom_symbol_text(name, lib_prefix):
    descr, left, right = design.SYMBOLS[name]
    rows = max(len(left), len(right))
    longest = max(len(p[1]) for p in left + right if p)
    hw = max(7.62, math.ceil((longest * 1.27 * 0.8 + 2) / GRID) * GRID)
    top = math.ceil((rows - 1) / 2) * GRID
    bottom = top - (rows - 1) * GRID
    body_top, body_bot = top + GRID, bottom - GRID
    full = lib_prefix + name
    ref = {"MSX_Cartridge": "J", "HDMI_A": "J", "Pico_Plus_2": "M"}.get(name, "U")
    font = "(effects (font (size 1.27 1.27)))"
    lines = [
        f"(symbol {q(full)} (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)",
        f"  (property \"Reference\" {q(ref)} (at {fnum(-hw)} {fnum(body_top + 1.27)} 0) (effects (font (size 1.27 1.27)) (justify left bottom)))",
        f"  (property \"Value\" {q(name)} (at {fnum(-hw)} {fnum(body_bot - 1.27)} 0) (effects (font (size 1.27 1.27)) (justify left top)))",
        f"  (property \"Footprint\" \"\" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))",
        f"  (property \"Datasheet\" \"\" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))",
        f"  (property \"Description\" {q(descr)} (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))",
        f"  (symbol {q(name + '_0_1')} (rectangle (start {fnum(-hw)} {fnum(body_top)}) (end {fnum(hw)} {fnum(body_bot)}) (stroke (width 0.254) (type default)) (fill (type background))))",
        f"  (symbol {q(name + '_1_1')}",
    ]
    for side, pins in (("L", left), ("R", right)):
        for i, p in enumerate(pins):
            if p is None:
                continue
            num, pname, etype = p
            y = top - i * GRID
            x = -hw - GRID if side == "L" else hw + GRID
            ang = 0 if side == "L" else 180
            lines.append(f"    (pin {etype} line (at {fnum(x)} {fnum(y)} {ang}) (length 2.54) "
                         f"(name {q(pname)} {font}) (number {q(num)} {font}))")
    lines.append("  )")
    lines.append(")")
    return "\n".join(lines)


def std_symbol_text(lib_id):
    lib, name = lib_id.split(":")
    text = open(os.path.join(KICAD_SYMBOLS, lib + ".kicad_sym")).read()
    block = find_block(text, f'(symbol "{name}"\n')
    return block.replace(f'(symbol "{name}"', f'(symbol "{lib_id}"', 1)


# Collect every lib symbol we need
lib_ids = sorted({c["symbol"] for c in design.COMPONENTS.values()})
lib_ids += ["power:GND", "power:+5V", "power:+3V3", "power:PWR_FLAG"]
lib_texts = {}
for lid in lib_ids:
    if lid.startswith(design.LIB + ":"):
        lib_texts[lid] = custom_symbol_text(lid.split(":")[1], design.LIB + ":")
    else:
        lib_texts[lid] = std_symbol_text(lid)


def symbol_pins(lid):
    """pin number -> (x, y, angle) in symbol coordinates (y up)."""
    node = parse(lib_texts[lid])[0]
    pins = {}
    for pin in find_all(node, "pin"):
        at = next(x for x in pin if isinstance(x, list) and x[0] == "at")
        num = next(x for x in pin if isinstance(x, list) and x[0] == "number")
        pins[unquote(num[1])] = (float(at[1]), float(at[2]), int(float(at[3])))
    return pins


# ---------------------------------------------------------------------------
# Placement (sheet coordinates, mm, y down). A3 = 420 x 297.
# ---------------------------------------------------------------------------
PLACE = {
    "J1": (55.88, 99.06),
    "U3": (139.7, 66.04), "U5": (139.7, 147.32),
    "U1": (218.44, 66.04), "U2": (218.44, 147.32), "U4": (218.44, 220.98),
    "M1": (304.8, 99.06),
    "J2": (381.0, 99.06),
}
for i in range(8):
    PLACE[f"R{i + 1}"] = (299.72 + i * 12.7, 185.42)
for i, ref in enumerate(["R9", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "D1"]):
    PLACE[ref] = (30.48 + i * 15.24, 248.92) if ref != "D1" else (30.48 + 9 * 15.24, 248.92)

items = []            # schematic body items
instances = []        # (reference) for sanity
points = {}           # connection point -> description, to catch accidental shorts
pwr_count = [0]


def claim(pt, what):
    key = (round(pt[0], 3), round(pt[1], 3))
    if key in points:
        raise SystemExit(f"connection point collision at {key}: {points[key]} / {what}")
    points[key] = what


def lib_property_pos(lib_id, prop, x, y, rot):
    """Sheet position and text angle of a library property after placement."""
    node = parse(lib_texts[lib_id])[0]
    for p in node:
        if isinstance(p, list) and p[0] == "property" and unquote(p[1]) == prop:
            at = next(e for e in p if isinstance(e, list) and e[0] == "at")
            lx, ly, la = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
            r = math.radians(rot)
            rx = lx * math.cos(r) - ly * math.sin(r)
            ry = lx * math.sin(r) + ly * math.cos(r)
            ang = (la + rot) % 360
            if ang == 180:
                ang = 0
            elif ang == 270:
                ang = 90
            return (x + rx, y - ry), ang
    return (x, y), 0


def place_symbol(lib_id, ref, value, x, y, rot=0, footprint="", mpn="", in_bom=True,
                 pins=(), hide_ref=False, descr=""):
    is_power = ref.startswith("#")
    ref_pos, ref_ang = lib_property_pos(lib_id, "Reference", x, y, rot)
    val_pos, val_ang = lib_property_pos(lib_id, "Value", x, y, rot)
    props = [
        ("Reference", ref, ref_pos, ref_ang, hide_ref),
        ("Value", value, val_pos, val_ang, False),
        ("Footprint", footprint, (x, y), 0, True),
        ("Datasheet", "", (x, y), 0, True),
        ("Description", descr, (x, y), 0, True),
    ]
    if mpn:
        props.append(("MPN", mpn, (x, y), 0, True))
    s = [f'(symbol (lib_id {q(lib_id)}) (at {fnum(x)} {fnum(y)} {rot}) (unit 1) '
         f'(exclude_from_sim no) (in_bom {"yes" if in_bom else "no"}) (on_board {"no" if is_power else "yes"}) (dnp no) '
         f'(uuid {q(uid())})']
    for name, val, (px, py), ang, hide in props:
        h = " (hide yes)" if hide else ""
        just = " (justify left)" if name in ("Reference", "Value") and lib_id.startswith(design.LIB + ":") else ""
        s.append(f'  (property {q(name)} {q(val)} (at {fnum(px)} {fnum(py)} {fnum(ang)}) '
                 f'(effects (font (size 1.27 1.27)){just}{h}))')
    for p in pins:
        s.append(f'  (pin {q(p)} (uuid {q(uid())}))')
    s.append(f'  (instances (project {q(design.PROJECT)} (path {q("/" + root_uuid)} '
             f'(reference {q(ref)}) (unit 1))))')
    s.append(")")
    items.append("\n".join(s))


def wire(a, b):
    items.append(f'(wire (pts (xy {fnum(a[0])} {fnum(a[1])}) (xy {fnum(b[0])} {fnum(b[1])})) '
                 f'(stroke (width 0) (type default)) (uuid {q(uid())}))')


def label(text, pt, direction):
    ang, just = {"L": (180, "right"), "R": (0, "left"),
                 "U": (90, "left"), "D": (270, "right")}[direction]
    items.append(f'(label {q(text)} (at {fnum(pt[0])} {fnum(pt[1])} {ang}) '
                 f'(effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid {q(uid())}))')


def global_label(text, pt, direction):
    ang, just = {"L": (180, "right"), "R": (0, "left"),
                 "U": (90, "left"), "D": (270, "right")}[direction]
    items.append(f'(global_label {q(text)} (shape passive) (at {fnum(pt[0])} {fnum(pt[1])} {ang}) '
                 f'(fields_autoplaced yes) (effects (font (size 1.27 1.27)) (justify {just})) (uuid {q(uid())}) '
                 f'(property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {fnum(pt[0])} {fnum(pt[1])} 0) '
                 f'(effects (font (size 1.27 1.27)) (hide yes))))')


def no_connect(pt):
    items.append(f'(no_connect (at {fnum(pt[0])} {fnum(pt[1])}) (uuid {q(uid())}))')


def power_symbol(net, pt, direction):
    pwr_count[0] += 1
    lid = {"GND": "power:GND", "+5V": "power:+5V", "+3V3": "power:+3V3"}[net]
    if net == "GND":
        rot = {"D": 0, "U": 180, "R": 90, "L": 270}[direction]
    else:
        rot = {"U": 0, "D": 180, "R": 270, "L": 90}[direction]
    place_symbol(lid, f"#PWR{pwr_count[0]:03d}", net, pt[0], pt[1], rot,
                 in_bom=False, pins=["1"], hide_ref=True)


def outward(angle):
    # pin angle points from the connection point toward the body
    return {0: "L", 180: "R", 90: "D", 270: "U"}[angle]


STEP = {"L": (-1, 0), "R": (1, 0), "U": (0, -1), "D": (0, 1)}

for ref, c in design.COMPONENTS.items():
    x0, y0 = PLACE[ref]
    pins = symbol_pins(c["symbol"])
    place_symbol(c["symbol"], ref, c["value"], x0, y0, 0, c["footprint"], c["mpn"],
                 in_bom=(ref != "J1"),          # the edge connector is part of the PCB
                 pins=sorted(pins), descr=c["description"])
    instances.append(ref)
    for num, (px, py, ang) in pins.items():
        end = (x0 + px, y0 - py)
        net = c["pins"].get(num)
        if net is None:
            claim(end, f"{ref}.{num} NC")
            no_connect(end)
            continue
        d = outward(ang)
        stub = (end[0] + STEP[d][0] * STUB, end[1] + STEP[d][1] * STUB)
        claim(end, f"{ref}.{num}")
        claim(stub, f"{ref}.{num} stub")
        wire(end, stub)
        if net in design.POWER_NETS and d in ("L", "R"):
            global_label(net, stub, d)      # side pins: compact, merges with the power net
        elif net in design.POWER_NETS:
            power_symbol(net, stub, d)
        else:
            label(net, stub, d)

# PWR_FLAGs
for i, net in enumerate(design.PWR_FLAG_NETS):
    pt = (330.2 + i * 15.24, 248.92)
    place_symbol("power:PWR_FLAG", f"#FLG{i + 1:02d}", "PWR_FLAG", pt[0], pt[1],
                 in_bom=False, pins=["1"], hide_ref=True)
    stub = (pt[0], pt[1] + STUB)
    claim(pt, "flag")
    claim(stub, "flag stub")
    wire(pt, stub)
    if net in design.POWER_NETS:
        power_symbol(net, stub, "D")
    else:
        label(net, stub, "D")


def text(s, x, y, size=2.0):
    items.append(f'(text {q(s)} (exclude_from_sim no) (at {fnum(x)} {fnum(y)} 0) '
                 f'(effects (font (size {size} {size})) (justify left bottom)) (uuid {q(uid())}))')


text("MSX V9990 (GFX9000 compatible) cartridge - RP2350 (Pimoroni Pico Plus 2)", 20.32, 20.32, 3)
text("Generated by hardware/gen/gen_schematic.py from design.py - edit the generator, not this file",
     20.32, 26.67, 1.5)
text("U3: /CS = 0 when A7..A4 = 0110 (ports 60h-6Fh), /IORQ = 0, /M1 = 1", 101.6, 35.56, 1.5)
text("U1: data bus, DIR = /RD (read: RP2350 -> MSX), enabled by /CS", 190.5, 35.56, 1.5)
text("U4: open-drain /INT, /WAIT, /BUSDIR (5V tolerant)", 190.5, 200.66, 1.5)
text("HDMI (DVI): 270R series resistors, ~500mV swing", 299.72, 172.72, 1.5)
text("Decoupling: one 100n per IC, 10u bulk on +5V and +3V3", 20.32, 236.22, 1.5)

# ---------------------------------------------------------------------------
# Write the schematic and the project symbol library
# ---------------------------------------------------------------------------
sch = [
    "(kicad_sch (version 20231120) (generator \"eeschema\") (generator_version \"8.0\")",
    f"  (uuid {q(root_uuid)})",
    "  (paper \"A3\")",
    "  (title_block (title \"MSX V9990 Cartridge (RP2350)\") (rev \"0.1\") "
    "(comment 1 \"Generated from hardware/gen/design.py\"))",
    "  (lib_symbols",
    *["    " + lib_texts[l].replace("\n", "\n    ") for l in lib_ids],
    "  )",
    *["  " + it.replace("\n", "\n  ") for it in items],
    "  (sheet_instances (path \"/\" (page \"1\")))",
    ")",
]
os.makedirs(out_dir, exist_ok=True)
with open(sch_path, "w") as f:
    f.write("\n".join(sch) + "\n")

with open(os.path.join(out_dir, design.LIB + ".kicad_sym"), "w") as f:
    f.write('(kicad_symbol_lib (version 20231120) (generator "kicad_symbol_editor") (generator_version "8.0")\n')
    for name in design.SYMBOLS:
        f.write("  " + custom_symbol_text(name, "").replace("\n", "\n  ") + "\n")
    f.write(")\n")

with open(os.path.join(out_dir, "sym-lib-table"), "w") as f:
    f.write('(sym_lib_table (version 7)\n'
            f'  (lib (name "{design.LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{design.LIB}.kicad_sym")(options "")(descr "Project symbols"))\n)\n')

print(f"wrote {sch_path}: {len(instances)} components, {pwr_count[0]} power symbols")

"""
Generate the PCB (placement, outline, silkscreen, rule areas) from the
netlist exported from the schematic. Routing is done afterwards by
route.py (Freerouting) and zones are filled by finish_pcb.py.

    python3 gen_pcb.py <project_dir> <netlist.net>
"""
import os
import sys
import uuid

sys.path.insert(0, "/usr/lib/python3.8/site-packages")
import pcbnew  # noqa: E402

import design  # noqa: E402
from sexp import find_all, parse, unquote  # noqa: E402

proj_dir, netlist_path = sys.argv[1], sys.argv[2]
pcb_path = os.path.join(proj_dir, design.PROJECT + ".kicad_pcb")
lib_dir = os.path.join(proj_dir, design.LIB + ".pretty")
KICAD_FP = "/usr/share/kicad/footprints"

MM = pcbnew.FromMM


def V(x, y):
    return pcbnew.VECTOR2I(MM(x), MM(y))


# ---------------------------------------------------------------------------
# Board geometry (mm). Fingers follow the cartridge PCB of costarc's
# MSX-Protoboard ("MSX_CART_SW_Official"), which is known to fit real slots;
# outline and shell-post holes follow MSXLegend's KC MSX FLASH V3 (made for
# the Overrich Konami-style cartridge case), aligned on the finger edge.
# ---------------------------------------------------------------------------
EDGE_PIN1_X = 105.682        # x of pins 1/2; pins 49/50 at 105.682 - 24 * 2.54
FINGER_Y = 78.808            # finger centre line
FINGER_W, FINGER_L = 1.5, 8.0

BOARD_BOTTOM = FINGER_Y + FINGER_L / 2   # fingers run right up to the bottom edge


def _corner(cx, cy, a0, a1, r=1.0, n=6):
    """Points of a rounded tongue corner (centre cx, cy), angles in degrees."""
    import math
    return [(round(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), 4),
             round(cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)), 4)) for i in range(n + 1)]


BOARD_TOP = 18.038
BOARD_LEFT, BOARD_RIGHT = 19.602, 120.692
TONGUE_TOP = 70.918
TONGUE_LEFT, TONGUE_RIGHT = 42.812, 107.597
OUTLINE = (
    [(BOARD_LEFT, BOARD_TOP), (BOARD_RIGHT, BOARD_TOP), (BOARD_RIGHT, TONGUE_TOP), (TONGUE_RIGHT, TONGUE_TOP)]
    + _corner(TONGUE_RIGHT - 1.0, BOARD_BOTTOM - 1.0, 0, 90)     # tongue bottom-right, R1
    + _corner(TONGUE_LEFT + 1.0, BOARD_BOTTOM - 1.0, 90, 180)    # tongue bottom-left, R1
    + [(TONGUE_LEFT, TONGUE_TOP), (30.112, TONGUE_TOP), (30.112, 59.567), (BOARD_LEFT, 59.567)]
)
HOLES = [(46.862, 61.318, 2.18),     # H2 (cartridge-shell post, 4.36mm cutout)
         (103.062, 66.718, 2.18)]    # H1
TONGUE = [(TONGUE_LEFT, TONGUE_TOP), (TONGUE_RIGHT, TONGUE_TOP), (TONGUE_RIGHT, BOARD_BOTTOM), (TONGUE_LEFT, BOARD_BOTTOM)]


def write_core2350b_footprint():
    """Waveshare Core2350B: 64 holes on a 10x10 2.54mm grid (outer two rings),
    four 2x8 headers in a pinwheel. Pads 1-16 = P1 (left), 17-32 = P2 (top),
    33-48 = P3 (bottom), 49-64 = P4 (right), unrotated, seen from the top."""
    os.makedirs(lib_dir, exist_ok=True)
    u = lambda: str(uuid.uuid4())
    font = "(effects (font (size 1 1) (thickness 0.15)))"
    lines = [
        '(footprint "Waveshare_Core2350B" (version 20240108) (generator "pcbnew") (generator_version "8.0") (layer "F.Cu")',
        '  (descr "Waveshare Core2350B RP2350B core board, 25.4x25.4mm, 64 pins on a 2.54mm grid '
        '(four 2x8 headers). https://www.waveshare.com/wiki/Core2350B0")',
        '  (tags "RP2350B Waveshare core board module")',
        f'  (property "Reference" "M1" (at 0 -14.2 0) (layer "F.SilkS") (uuid "{u()}") {font})',
        f'  (property "Value" "Waveshare_Core2350B" (at 0 14.2 0) (layer "F.Fab") (uuid "{u()}") {font})',
        f'  (property "Footprint" "" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid "{u()}") {font})',
        f'  (property "Datasheet" "" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid "{u()}") {font})',
        f'  (property "Description" "" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid "{u()}") {font})',
        '  (attr through_hole)',
    ]
    for header in range(4):
        for i in range(16):
            n = header * 16 + i + 1
            k, odd = i // 2, (i % 2 == 0)
            if header == 0:   # P1 left
                c, r = (0 if odd else 1), k
            elif header == 1:  # P2 top
                c, r = 9 - k, (0 if odd else 1)
            elif header == 2:  # P3 bottom
                c, r = k, (9 if odd else 8)
            else:              # P4 right
                c, r = (9 if odd else 8), 9 - k
            x, y = (c - 4.5) * 2.54, (r - 4.5) * 2.54
            shape = "rect" if i == 0 else "circle"
            lines.append(f'  (pad "{n}" thru_hole {shape} (at {x:.3f} {y:.3f}) (size 1.7 1.7) (drill 1.0) '
                         f'(layers "*.Cu" "*.Mask") (remove_unused_layers no) (uuid "{u()}"))')
    for layer, h, w in (("F.Fab", 12.7, 0.1), ("F.SilkS", 13.0, 0.12), ("F.CrtYd", 13.2, 0.05)):
        lines.append(f'  (fp_rect (start -{h} -{h}) (end {h} {h}) (stroke (width {w}) (type default)) '
                     f'(fill none) (layer "{layer}") (uuid "{u()}"))')
    for txt, x, y in (("P1", -9.5, 0), ("P2", 0, -9.5), ("P3", 0, 9.5), ("P4", 9.5, 0)):
        lines.append(f'  (fp_text user "{txt}" (at {x} {y} 0) (layer "F.Fab") (uuid "{u()}") {font})')
    lines.append(f'  (fp_text user "Core2350B" (at 0 0 0) (layer "F.Fab") (uuid "{u()}") {font})')
    lines.append(")")
    with open(os.path.join(lib_dir, "Waveshare_Core2350B.kicad_mod"), "w") as f:
        f.write("\n".join(lines) + "\n")


def write_edge_footprint():
    """MSX edge connector: odd pins on B.Cu, even on F.Cu, pin 1/2 on the right
    when viewed from the front. Pin 44 (SW1) is 1mm shorter at the edge side."""
    os.makedirs(lib_dir, exist_ok=True)
    u = lambda: str(uuid.uuid4())
    font = "(effects (font (size 1 1) (thickness 0.15)))"
    lines = [
        '(footprint "MSX_Cartridge_Edge" (version 20240108) (generator "pcbnew") (generator_version "8.0") (layer "F.Cu")',
        '  (descr "MSX cartridge edge connector, 50 contacts, 2.54mm pitch. Odd pins on B.Cu, even pins on F.Cu, '
        'pins 1/2 on the right seen from the component side. Order with gold fingers + 45 deg bevel.")',
        '  (tags "MSX cartridge edge connector")',
        f'  (property "Reference" "J1" (at -34 -3 0) (layer "F.SilkS") (uuid "{u()}") {font})',
        f'  (property "Value" "MSX_Cartridge_Edge" (at 0 6 0) (layer "F.Fab") (uuid "{u()}") {font})',
        f'  (property "Footprint" "" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid "{u()}") {font})',
        f'  (property "Datasheet" "" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid "{u()}") {font})',
        f'  (property "Description" "" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid "{u()}") {font})',
        '  (attr exclude_from_pos_files exclude_from_bom allow_missing_courtyard)',
    ]
    for n in range(1, 51):
        k = (n - 1) // 2
        x = (12 - k) * 2.54
        layer = '"B.Cu" "B.Mask"' if n % 2 else '"F.Cu" "F.Mask"'
        length, y = (FINGER_L - 1.0, -0.5) if n == 44 else (FINGER_L, 0.0)
        lines.append(f'  (pad "{n}" connect rect (at {x:.3f} {y}) (size {FINGER_W} {length}) '
                     f'(layers {layer}) (uuid "{u()}"))')
    # pin markers on the silkscreen above the fingers
    for txt, x, layer, mirror in (("2", 30.48, "F.SilkS", ""), ("50", -30.48, "F.SilkS", ""),
                                  ("1", 30.48, "B.SilkS", " (justify mirror)"),
                                  ("49", -30.48, "B.SilkS", " (justify mirror)")):
        lines.append(f'  (fp_text user "{txt}" (at {x} -5.6 0) (layer "{layer}") (uuid "{u()}") '
                     f'(effects (font (size 1 1) (thickness 0.15)){mirror}))')
    lines.append(f'  (fp_rect (start -31.5 -4.2) (end 31.5 4.2) (stroke (width 0.05) (type default)) '
                 f'(fill none) (layer "F.CrtYd") (uuid "{u()}"))')
    lines.append(")")
    with open(os.path.join(lib_dir, "MSX_Cartridge_Edge.kicad_mod"), "w") as f:
        f.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------
# Placement: ref -> (x, y, rotation_deg)   (all parts on the front side)
# ---------------------------------------------------------------------------
PLACE = {
    "J1": (EDGE_PIN1_X - 12 * 2.54, FINGER_Y, 0),
    "M1": (21.87, 44.89, 90),        # origin = pin 1; module centre (46, 36), USB-C at the left edge
    "J2": (84.0, BOARD_TOP + 3.5, 180),     # HDMI opening at the top edge
    "U1": (60.0, 62.0, 90),          # data transceiver above D0-D7 fingers
    "U2": (76.0, 62.0, 90),          # addr/ctrl buffer
    "U3": (92.0, 62.0, 90),          # port decoder
    "U4": (111.5, 59.0, 90),         # open-drain drivers
    "U5": (111.5, 47.5, 90),         # BUSDIR OR gate
    # power parts in the strip between the board top edge and the Pico
    "D1": (27.2, 22.4, 0),           # cathode (VSYS) next to Pico pin 39
    "C6": (31.4, 22.4, 0),           # +5V bulk
    "C7": (37.2, 22.4, 0),           # +3V3 bulk, near Pico pin 36
    "R9": (100.5, 57.5, 90),
    "R10": (102.7, 57.5, 90),        # SW1 hold resistors, next to U3
    "R11": (104.9, 57.5, 90),
    # VDP select switch at the top edge, right of the HDMI connector; rotated
    # so the lever sticks ~1.2mm out of the top edge (origin = pin 1; the far
    # pad row keeps 0.5mm from the edge)
    "SW1": (108.5, BOARD_TOP + 4.5, 180),
}
# DVI series resistors in HDMI pad order (D2+ D2- D1+ D1- D0+ D0- CK+ CK-)
for i, ref in enumerate(["R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"]):
    PLACE[ref] = (73.5 + i * 1.8, 39.0, 90)     # right end stays clear of HDMI pin 18 (+5V)
if design.VARIANT == "core2350b":
    # Waveshare Core2350B, rotated so P2 (VBUS/3V3, unused USB/SWD) faces the
    # left edge, P1 (GP0-11) faces the bus ICs and P3 (DVI, control) faces right,
    # leaving a channel between the module and the HDMI resistors.
    PLACE.update({
        "M1": (50.0, 36.0, 90),          # footprint origin = module centre, bus pins above U1/U2
        "J2": (80.0, BOARD_TOP + 3.5, 180),
        "D1": (31.0, 40.0, 0),           # MSX +5V -> module VBUS (left of the module)
        "C6": (31.0, 36.5, 0),           # +5V bulk
        "C7": (35.0, 43.62, 180),        # +3V3 bulk, fed by a fixed track from the 3V3 pin
    })
    for i, ref in enumerate(["R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"]):
        PLACE[ref] = (69.6 + i * 1.8, 39.0, 90)
# Decoupling caps: next to each IC's VCC pin
DECOUPLE = {"C1": "U1", "C2": "U2", "C3": "U3", "C4": "U4", "C5": "U5"}


def load_netlist(path):
    root = parse(open(path).read())[0]
    comps = {}
    for c in find_all(root, "comp"):
        get = lambda k: next((x for x in c if isinstance(x, list) and x[0] == k), None)
        ref = unquote(get("ref")[1])
        fields = {}
        f = get("fields")
        if f:
            for fl in f[1:]:
                if isinstance(fl, list) and len(fl) > 2:
                    fields[unquote(fl[1][1])] = unquote(fl[2])
        comps[ref] = dict(value=unquote(get("value")[1]), footprint=unquote(get("footprint")[1]),
                          tstamp=unquote(get("tstamps")[1]), fields=fields)
    nets = {}
    for n in find_all(root, "net"):
        name = unquote(next(x for x in n if isinstance(x, list) and x[0] == "name")[1])
        nodes = []
        for nd in find_all(n, "node"):
            g = {x[0]: unquote(x[1]) for x in nd[1:] if isinstance(x, list)}
            nodes.append((g["ref"], g["pin"]))
        nets[name] = nodes
    return comps, nets


def load_footprint(fpid):
    lib, name = fpid.split(":")
    path = lib_dir if lib == design.LIB else os.path.join(KICAD_FP, lib + ".pretty")
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        raise SystemExit(f"footprint not found: {fpid}")
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def add_segment(board, a, b, layer=pcbnew.Edge_Cuts, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(V(*a))
    s.SetEnd(V(*b))
    s.SetLayer(layer)
    s.SetWidth(MM(width))
    board.Add(s)


def add_circle(board, c, r, layer=pcbnew.Edge_Cuts, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_CIRCLE)
    s.SetCenter(V(*c))
    s.SetEnd(V(c[0] + r, c[1]))
    s.SetLayer(layer)
    s.SetWidth(MM(width))
    board.Add(s)


def add_text(board, txt, x, y, size=1.0, layer=pcbnew.F_SilkS, angle=0, bold=False):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(txt)
    t.SetPosition(V(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
    t.SetTextThickness(MM(max(0.15, size * (0.2 if bold else 0.15))))
    t.SetTextAngleDegrees(angle)
    if layer in (pcbnew.B_SilkS, pcbnew.B_Cu):
        t.SetMirrored(True)
    board.Add(t)


def add_rule_area(board, pts, layers, no_vias=True, no_tracks=False, no_pour=True, name=""):
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowVias(no_vias)
    z.SetDoNotAllowTracks(no_tracks)
    z.SetDoNotAllowCopperPour(no_pour)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    ls = pcbnew.LSET()
    for l in layers:
        ls.AddLayer(l)
    z.SetLayerSet(ls)
    z.SetZoneName(name)
    outline = z.Outline()
    outline.NewOutline()
    for p in pts:
        outline.Append(MM(p[0]), MM(p[1]))
    board.Add(z)


def main():
    write_edge_footprint()
    if design.VARIANT == "core2350b":
        write_core2350b_footprint()
    comps, nets = load_netlist(netlist_path)

    board = pcbnew.NewBoard(pcb_path)
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(MM(1.6))
    board.SetCopperLayerCount(2)

    net_items = {}
    for name in nets:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        net_items[name] = ni

    fps = {}
    for ref, c in comps.items():
        fp = load_footprint(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH("/" + c["tstamp"]))
        fp.SetSheetname("Root")
        fp.SetSheetfile(design.PROJECT + ".kicad_sch")
        if "MPN" in c["fields"]:
            fp.SetField("MPN", c["fields"]["MPN"])
            fp.GetFieldByName("MPN").SetVisible(False)
        board.Add(fp)
        fps[ref] = fp

    for name, nodes in nets.items():
        for ref, pin in nodes:
            for pad in fps[ref].Pads():
                if pad.GetNumber() == pin:
                    pad.SetNet(net_items[name])

    for ref, (x, y, rot) in PLACE.items():
        fps[ref].SetPosition(V(x, y))
        fps[ref].SetOrientationDegrees(rot)

    # decoupling caps just outside the VCC pin of their IC
    vcc_pin = {"U1": "20", "U2": "20", "U3": "20", "U4": "14", "U5": "14"}
    for cref, uref in DECOUPLE.items():
        pad = next(p for p in fps[uref].Pads() if p.GetNumber() == vcc_pin[uref])
        px, py = pcbnew.ToMM(pad.GetPosition().x), pcbnew.ToMM(pad.GetPosition().y)
        fps[cref].SetPosition(V(px + 1.0, py - 3.2))
        fps[cref].SetOrientationDegrees(0)

    # Reference designators: small; the DVI resistor row gets them below the parts
    for ref, fp in fps.items():
        t = fp.Reference()
        t.SetTextSize(pcbnew.VECTOR2I(MM(0.8), MM(0.8)))
        t.SetTextThickness(MM(0.15))
        if ref in ("M1", "J2"):
            fp.Value().SetVisible(False)        # own silkscreen labels are placed instead
        if ref == "J2":
            t.SetVisible(False)                 # the "HDMI" label sits there
        if ref == "D1" and design.VARIANT == "picoplus2":
            t.SetPosition(V(PLACE["D1"][0] - 3.6, PLACE["D1"][1]))   # keep off the board edge
        if ref in ("R9", "R10", "R11"):
            x, y, _ = PLACE[ref]
            t.SetPosition(V(x, y - 2.5))
            t.SetTextAngleDegrees(90)
        if ref in ("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"):
            x, y, _ = PLACE[ref]
            t.SetPosition(V(x, y + 2.7))
            t.SetTextAngleDegrees(90)

    # SW1 sits at the board edge where a thermal relief only gets one spoke:
    # connect its GND pads solidly (they also tie the two GND pours together)
    for pad in fps["SW1"].Pads():
        if pad.GetNetname() == "GND":
            pad.SetZoneConnection(pcbnew.ZONE_CONNECTION_FULL)

    # outline + holes
    for a, b in zip(OUTLINE, OUTLINE[1:] + OUTLINE[:1]):
        add_segment(board, a, b)
    for x, y, r in HOLES:
        add_circle(board, (x, y), r)

    # keep copper 0.3mm+ away from the cartridge-shell holes (Freerouting does
    # not apply the board-edge clearance to them)
    import math
    for x, y, r in HOLES:
        pts = [(x + (r + 0.45) * math.cos(a * math.pi / 12), y + (r + 0.45) * math.sin(a * math.pi / 12))
               for a in range(24)]
        add_rule_area(board, pts, [pcbnew.F_Cu, pcbnew.B_Cu], no_tracks=True, name="hole_keepout")

    # Temporary keepouts (removed by finish_pcb.py) reserving room for the GND
    # copper added after autorouting: the HDMI ground bar under the connector
    # body and the GND finger stubs (pins 41/43, B.Cu) up to the pour.
    add_rule_area(board, [(75.0, BOARD_TOP), (93.0, BOARD_TOP), (93.0, BOARD_TOP + 7.45), (75.0, BOARD_TOP + 7.45)],
                  [pcbnew.F_Cu], no_tracks=True, no_pour=False, name="tmp_hdmi_gnd")
    for pin in (41, 43):
        x = EDGE_PIN1_X - ((pin - 1) // 2) * 2.54
        add_rule_area(board, [(x - 0.9, 69.5), (x + 0.9, 69.5), (x + 0.9, 75.0), (x - 0.9, 75.0)],
                      [pcbnew.B_Cu], no_tracks=True, no_pour=False, name=f"tmp_gnd_finger_{pin}")

    import pcb_helpers
    # HDMI pads (0.5mm pitch) -> series resistors: fixed 45-degree fan-out.
    # Traces bending the same way start their diagonal 0.5mm apart in y (the
    # outermost first) so no two diagonals run in parallel too closely.
    fan = []
    for ref in ("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"):
        rpad = next(p for p in fps[ref].Pads() if p.GetNumber() == "2")
        hpad = next(p for p in fps["J2"].Pads() if p.GetNetname() == rpad.GetNetname())
        fan.append((pcbnew.ToMM(hpad.GetPosition().x), pcbnew.ToMM(hpad.GetPosition().y),
                    pcbnew.ToMM(rpad.GetPosition().x), pcbnew.ToMM(rpad.GetPosition().y),
                    rpad.GetNet()))
    left = sorted((f for f in fan if f[2] < f[0]), key=lambda f: f[0])
    right = sorted((f for f in fan if f[2] >= f[0]), key=lambda f: -f[0])
    for group in (left, right):
        for k, (hx, hy, rx, ry, ni) in enumerate(group):
            y0 = hy + 1.4 + 0.5 * k
            y1 = y0 + abs(rx - hx)
            pcb_helpers.add_track(board, (hx, hy), (hx, y0), ni, 0.25)
            pcb_helpers.add_track(board, (hx, y0), (rx, y1), ni, 0.25)
            pcb_helpers.add_track(board, (rx, y1), (rx, ry), ni, 0.25)

    # HDMI +5V (pin 18) sits right of the TMDS fan-out and keeps losing the
    # autorouting lottery: give it a fixed path to C5's +5V pad (next to U5).
    p18 = next(p for p in fps["J2"].Pads() if p.GetNumber() == "18")
    c5 = next(p for p in fps["C5"].Pads() if p.GetNetname() == "+5V")
    ax, ay = pcbnew.ToMM(p18.GetPosition().x), pcbnew.ToMM(p18.GetPosition().y)
    cx, cy = pcbnew.ToMM(c5.GetPosition().x), pcbnew.ToMM(c5.GetPosition().y)
    run_y = ay + 4.25
    path = [(ax, ay), (ax, ay + 2.0), (ax + 2.25, run_y), (cx - 2.0, run_y), (cx, run_y + 2.0), (cx, cy)]
    for i, (a, b) in enumerate(zip(path, path[1:])):
        # narrow where it leaves the 0.5mm-pitch pad row
        pcb_helpers.add_track(board, a, b, p18.GetNet(), 0.25 if i == 0 else 0.4)

    if design.VARIANT == "picoplus2":
        # MSX +5V reaches D1/C6 only between two Pico pins (0.84mm gap, which
        # Freerouting's inflated clearance often misses): fix that stretch.
        c6 = next(p for p in fps["C6"].Pads() if p.GetNetname() == "+5V")
        cx, cy = pcbnew.ToMM(c6.GetPosition().x), pcbnew.ToMM(c6.GetPosition().y)
        gx = PLACE["M1"][0] + 3.5 * 2.54        # midway between pin columns 4 and 5
        pcb_helpers.add_track(board, (cx, cy), (gx, cy + 0.31), c6.GetNet(), 0.4)
        pcb_helpers.add_track(board, (gx, cy + 0.31), (gx, cy + 1.31), c6.GetNet(), 0.4)
        pcb_helpers.add_via(board, (gx, cy + 1.31), c6.GetNet())
        pcb_helpers.add_track(board, (gx, cy + 1.31), (gx, 50.5), c6.GetNet(), 0.4, layer=pcbnew.B_Cu)

    if design.VARIANT == "core2350b":
        # The module's 3V3 pin is on the inner ring; power-width tracks cannot
        # pass between the outer-ring pins (0.84mm gap). Leave with 0.3mm
        # between two rows of pins to C7 just outside the module.
        p3 = next(p for p in fps["M1"].Pads() if p.GetNetname() == "+3V3")
        c7 = next(p for p in fps["C7"].Pads() if p.GetNetname() == "+3V3")
        ax, ay = pcbnew.ToMM(p3.GetPosition().x), pcbnew.ToMM(p3.GetPosition().y)
        cx, cy = pcbnew.ToMM(c7.GetPosition().x), pcbnew.ToMM(c7.GetPosition().y)
        pcb_helpers.add_track(board, (ax, ay), (ax, cy), p3.GetNet(), 0.3)
        pcb_helpers.add_track(board, (ax, cy), (cx, cy), p3.GetNet(), 0.3)

    # reserve the GND fan-out stubs/vias (added by finish_pcb.py)
    import pcb_helpers
    for i, (pad, via) in enumerate(pcb_helpers.gnd_fanout_points(board)):
        px, py = pcbnew.ToMM(pad.x), pcbnew.ToMM(pad.y)
        vx, vy = pcbnew.ToMM(via.x), pcbnew.ToMM(via.y)
        h = 0.45
        add_rule_area(board, [(min(px, vx) - h, min(py, vy) - h), (max(px, vx) + h, min(py, vy) - h),
                              (max(px, vx) + h, max(py, vy) + h), (min(px, vx) - h, max(py, vy) + h)],
                      [pcbnew.F_Cu], no_tracks=True, no_pour=False, name=f"tmp_fanout_{i}")
        add_rule_area(board, [(vx - h, vy - h), (vx + h, vy - h), (vx + h, vy + h), (vx - h, vy + h)],
                      [pcbnew.B_Cu], no_tracks=True, no_pour=False, name=f"tmp_fanout_b_{i}")

    # Gold fingers may only be entered from above: no tracks between the
    # fingers, beside the outer fingers, or below their ends.
    both = [pcbnew.F_Cu, pcbnew.B_Cu]
    top = FINGER_Y - FINGER_L / 2 - 0.3
    for k in range(24):
        gx = EDGE_PIN1_X - 2.54 * k - 1.27
        add_rule_area(board, [(gx - 0.3, top), (gx + 0.3, top), (gx + 0.3, 84.5), (gx - 0.3, 84.5)],
                      both, no_tracks=True, name=f"finger_gap_{k}")
    for x0, x1 in ((EDGE_PIN1_X + 1.05, 109.0), (41.0, EDGE_PIN1_X - 24 * 2.54 - 1.05)):
        add_rule_area(board, [(x0, top), (x1, top), (x1, 84.5), (x0, 84.5)],
                      both, no_tracks=True, name="finger_side")
    bottom = FINGER_Y + FINGER_L / 2 + 0.1
    add_rule_area(board, [(41.0, bottom), (109.0, bottom), (109.0, 84.5), (41.0, 84.5)],
                  both, no_tracks=True, name="finger_tips")

    # no vias and no copper pour on the gold-finger tongue (both sides)
    add_rule_area(board, TONGUE, [pcbnew.F_Cu, pcbnew.B_Cu], name="gold_fingers")

    # silkscreen
    sx = PLACE["SW1"][0]
    add_text(board, "V9968", sx - 7.5, PLACE["SW1"][1] + 2.5, 0.8)        # slider to the left
    add_text(board, "V9990", sx + 0.2, PLACE["SW1"][1] + 2.5, 0.8)        # slider to the right
    add_text(board, "MSX V9990/V9968", 106.0, 29.6, 1.3, bold=True)
    add_text(board, "CARTRIDGE (RP2350)", 106.0, 31.9, 1.0)
    add_text(board, "rev 0.3", 106.0, 34.0, 0.9)
    add_text(board, "hoony9845@gmail.com", 106.0, 36.0, 0.9)
    hx = PLACE["J2"][0]
    add_text(board, "HDMI", hx, 31.0, 1.0)
    if design.VARIANT == "picoplus2":
        add_text(board, "USB", 23.5, 49.0, 0.9)
        add_text(board, "Pimoroni Pico Plus 2", 46.0, 49.0, 0.9)
    else:
        add_text(board, "Waveshare Core2350B (on pin headers)", 50.0, 50.2, 0.9)
    add_text(board, "LABEL SIDE / PINS 2-50", 75.2, 72.0, 0.9)
    add_text(board, "PINS 1-49", 75.2, 72.0, 0.9, layer=pcbnew.B_SilkS)
    add_text(board, "Generated by hardware/gen", 75.0, 40.0, 0.9, layer=pcbnew.B_SilkS)

    pcbnew.SaveBoard(pcb_path, board)

    # report key positions to sanity-check rotations
    for ref, pin in (("M1", "1"), ("M1", "16"), ("M1", "21"), ("M1", "39"),
                     ("J2", "1"), ("J2", "19"), ("U1", "1"), ("J1", "1"), ("J1", "50")):
        pad = next(p for p in fps[ref].Pads() if p.GetNumber() == pin)
        print(f"{ref}.{pin}: ({pcbnew.ToMM(pad.GetPosition().x):.2f}, "
              f"{pcbnew.ToMM(pad.GetPosition().y):.2f}) {pad.GetNetname()}")
    print("wrote", pcb_path)


main()

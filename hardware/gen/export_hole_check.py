"""
1:1 printable PDF for checking hole positions: board outline, pads, holes
at actual size, component outlines, numbered mechanical holes, a hole table
(coordinates from the board's top-left corner) and scale rulers.

    python3 export_hole_check.py <project_dir> <out.pdf>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, "/usr/lib/python3.8/site-packages")
import pcbnew  # noqa: E402

import design  # noqa: E402

proj_dir, out_pdf = sys.argv[1], sys.argv[2]
board = pcbnew.LoadBoard(os.path.join(proj_dir, design.PROJECT + ".kicad_pcb"))
MM, mm = pcbnew.FromMM, pcbnew.ToMM
L = pcbnew.Dwgs_User

# board extents from the outline geometry itself (not including line width)
pts = []
for d in board.GetDrawings():
    if d.GetLayer() == pcbnew.Edge_Cuts and d.GetShape() == pcbnew.SHAPE_T_SEGMENT:
        pts += [(mm(d.GetStart().x), mm(d.GetStart().y)), (mm(d.GetEnd().x), mm(d.GetEnd().y))]
ox, oy = min(p[0] for p in pts), min(p[1] for p in pts)          # board top-left = (0, 0)
bw, bh = max(p[0] for p in pts) - ox, max(p[1] for p in pts) - oy


def V(x, y):
    return pcbnew.VECTOR2I(MM(x), MM(y))


def line(a, b, w=0.15):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(V(*a))
    s.SetEnd(V(*b))
    s.SetLayer(L)
    s.SetWidth(MM(w))
    board.Add(s)


def circle(c, r, w=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_CIRCLE)
    s.SetCenter(V(*c))
    s.SetEnd(V(c[0] + r, c[1]))
    s.SetLayer(L)
    s.SetWidth(MM(w))
    board.Add(s)


def text(t, x, y, size=1.5, left=True):
    p = pcbnew.PCB_TEXT(board)
    p.SetText(t)
    p.SetPosition(V(x, y))
    p.SetLayer(L)
    p.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
    p.SetTextThickness(MM(size * 0.13))
    if left:
        p.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    board.Add(p)


# ---- collect holes -------------------------------------------------------
holes = []      # (label, x, y, diameter, kind)
n = 0
for d in board.GetDrawings():
    if d.GetLayer() == pcbnew.Edge_Cuts and d.GetShape() == pcbnew.SHAPE_T_CIRCLE:
        n += 1
        c = d.GetCenter()
        holes.append((f"H{n}", mm(c.x), mm(c.y), 2 * mm(d.GetRadius()), "cartridge shell post (routed)"))

for fp in sorted(board.GetFootprints(), key=lambda f: f.GetReference()):
    for p in fp.Pads():
        if p.GetDrillSize().x <= 0:
            continue
        holes.append((f"{fp.GetReference()}.{p.GetNumber()}", mm(p.GetPosition().x),
                      mm(p.GetPosition().y), mm(p.GetDrillSize().x),
                      "plated" if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else "non-plated"))

# ---- outline drawing of every drilled pad (pad ring + hole + centre cross)
for fp in board.GetFootprints():
    for p in fp.Pads():
        if p.GetDrillSize().x <= 0:
            continue
        x, y = mm(p.GetPosition().x), mm(p.GetPosition().y)
        circle((x, y), mm(p.GetSize().x) / 2, 0.08)
        circle((x, y), mm(p.GetDrillSize().x) / 2, 0.12)
        line((x - 0.3, y), (x + 0.3, y), 0.05)
        line((x, y - 0.3), (x, y + 0.3), 0.05)
# gold fingers (front side)
for p in next(f for f in board.GetFootprints() if f.GetReference() == "J1").Pads():
    if p.IsOnLayer(pcbnew.F_Cu):
        x, y = mm(p.GetPosition().x), mm(p.GetPosition().y)
        w, h = mm(p.GetSize().x) / 2, mm(p.GetSize().y) / 2
        for a, b in (((x - w, y - h), (x + w, y - h)), ((x + w, y - h), (x + w, y + h)),
                     ((x + w, y + h), (x - w, y + h)), ((x - w, y + h), (x - w, y - h))):
            line(a, b, 0.1)

# ---- annotate mechanical holes on the board -----------------------------
for label, x, y, dia, kind in holes:
    if label.startswith("H"):
        r = dia / 2 + 1.0
        line((x - r, y), (x + r, y), 0.1)
        line((x, y - r), (x, y + r), 0.1)
        text(label, x + dia / 2 + 0.6, y - dia / 2 - 0.4, 1.5)
pico = {h[0]: h for h in holes if h[0].startswith("M1.")}
if design.VARIANT == "core2350b":
    MODULE_PINS = {"1": "P1-1", "17": "P2-1", "33": "P3-1", "49": "P4-1"}
    MODULE_NOTE = "M1: Core2350B, 64 holes on a 2.54 mm grid (four 2x8 headers)"
else:
    MODULE_PINS = {"1": "pin 1", "20": "pin 20", "21": "pin 21", "40": "pin 40"}
    MODULE_NOTE = "M1: 40 holes, 2 rows x 20, pitch 2.54 mm, rows 17.78 mm apart"
for pin, name in MODULE_PINS.items():
    _, x, y, _, _ = pico[f"M1.{pin}"]
    text(f"M1 {name}", x - 2.5, y - 1.6, 1.0)

# ---- rulers (check the printer scale!) -----------------------------------
ry = oy + bh + 14
line((ox, ry), (ox + 100, ry), 0.2)
for i in range(0, 101, 5):
    t = 3.0 if i % 10 == 0 else 1.5
    line((ox + i, ry), (ox + i, ry - t), 0.15)
    if i % 10 == 0:
        text(str(i), ox + i - 0.8, ry + 2.5, 1.5)
text("100 mm - print at 100% (actual size) and check this ruler first", ox, ry + 5.5, 1.6)
rx = ox - 8
line((rx, oy), (rx, oy + 60), 0.2)
for i in range(0, 61, 5):
    t = 3.0 if i % 10 == 0 else 1.5
    line((rx, oy + i), (rx + t, oy + i), 0.15)
    if i % 10 == 0:
        text(str(i), rx - 4.5, oy + i + 0.6, 1.5)

# origin marker
line((ox - 3, oy), (ox + 3, oy), 0.1)
line((ox, oy - 3), (ox, oy + 3), 0.1)
text("(0,0)", ox - 1.0, oy - 2.0, 1.2)

# ---- hole table -----------------------------------------------------------
tx, ty = ox, oy + bh + 30          # below the rulers, well inside the printable area
text("MSX V9990/V9968 cartridge rev 0.2 - hole check (1:1, front / component side view)", tx, ty, 1.8)
ty += 4
text(f"Board {bw:.2f} x {bh:.2f} mm, origin = top-left corner, X right, Y down", tx, ty, 1.4)
ty += 6
text("Hole        X (mm)    Y (mm)   Dia (mm)  Type", tx, ty, 1.4)
ty += 1.2
line((tx, ty), (tx + 120, ty), 0.15)
ty += 3


def row(label, x, y, dia, kind):
    global ty
    text(f"{label:<10}{x - ox:>8.2f}{y - oy:>10.2f}{dia:>10.2f}   {kind}", tx, ty, 1.3)
    ty += 2.6


for h in holes:
    if h[0].startswith("H"):
        row(*h)
for h in holes:
    if h[0].startswith("J2."):
        row(h[0], h[1], h[2], h[3], "HDMI shield, " + h[4])
for pin, name in MODULE_PINS.items():
    h = pico[f"M1.{pin}"]
    row(h[0], h[1], h[2], h[3], f"module {name}, " + h[4])
ty += 1
text(MODULE_NOTE, tx, ty, 1.3)
ty += 2.6
text("Vias (0.3 mm) are not shown.", tx, ty, 1.3)
ty += 2.6
text("Gold fingers: pitch 2.54 mm, 1.5 x 8 mm, pins 2..50 shown (front)", tx, ty, 1.3)

with tempfile.TemporaryDirectory() as tmp:
    tmp_pcb = os.path.join(tmp, "hole_check.kicad_pcb")
    pcbnew.SaveBoard(tmp_pcb, board)
    subprocess.run(["kicad-cli", "pcb", "export", "pdf", "-o", out_pdf,
                    "--layers", "Edge.Cuts,F.Paste,F.Fab,Dwgs.User",
                    "--black-and-white", "--drill-shape-opt", "0",
                    "--exclude-value", tmp_pcb], check=True, stdout=subprocess.DEVNULL)
print("wrote", out_pdf, f"({len(holes)} holes)")

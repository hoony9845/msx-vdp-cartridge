"""
Add GND pours on both layers and fill all zones.

    python3 finish_pcb.py <project_dir>
"""
import os
import sys

sys.path.insert(0, "/usr/lib/python3.8/site-packages")
import pcbnew  # noqa: E402

import design  # noqa: E402

proj_dir = sys.argv[1]
pcb_path = os.path.join(proj_dir, design.PROJECT + ".kicad_pcb")
MM = pcbnew.FromMM

board = pcbnew.LoadBoard(pcb_path)
gnd = board.FindNet("GND")

# HDMI ground bar (see gen_pcb.prerouted_hdmi_ground); GND is not autorouted
import importlib.util
spec = importlib.util.spec_from_file_location("gp_helpers", os.path.join(os.path.dirname(__file__), "pcb_helpers.py"))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
for t in list(board.GetTracks()):
    # GND is never autorouted, so every GND track/via is ours from a previous run
    if t.GetNetname() == "GND":
        board.Remove(t)
for z in list(board.Zones()):
    if z.GetIsRuleArea() and z.GetZoneName().startswith("tmp_"):
        board.Remove(z)
helpers.prerouted_hdmi_ground(board, next(f for f in board.GetFootprints() if f.GetReference() == "J2"))

# GND fingers (B.Cu, inside the no-pour tongue) -> straight up into the B.Cu pour
gnd_net = board.FindNet("GND")
j1 = next(f for f in board.GetFootprints() if f.GetReference() == "J1")
for p in j1.Pads():
    if p.GetNetname() == "GND":
        x = pcbnew.ToMM(p.GetPosition().x)
        top = pcbnew.ToMM(p.GetPosition().y) - 3.5
        helpers.add_track(board, (x, top), (x, 70.0), gnd_net, 0.6, pcbnew.B_Cu)

# GND fan-out of SMD pads (space reserved by gen_pcb.py)
for pad, via in helpers.gnd_fanout_points(board):
    helpers.add_track(board, (pcbnew.ToMM(pad.x), pcbnew.ToMM(pad.y)),
                      (pcbnew.ToMM(via.x), pcbnew.ToMM(via.y)), gnd_net, 0.3)
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(via)
    v.SetWidth(MM(0.6))
    v.SetDrill(MM(0.3))
    v.SetNet(gnd_net)
    board.Add(v)

# drop pours from a previous run
for z in list(board.Zones()):
    if not z.GetIsRuleArea():
        board.Remove(z)

bbox = board.GetBoardEdgesBoundingBox()
x0, y0 = bbox.GetX() - MM(1), bbox.GetY() - MM(1)
x1, y1 = bbox.GetRight() + MM(1), bbox.GetBottom() + MM(1)

for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(gnd)
    z.SetZoneName("GND_" + board.GetLayerName(layer))
    z.SetLocalClearance(MM(0.3))
    z.SetMinThickness(MM(0.35))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)   # SMD pads: solid
    z.SetThermalReliefGap(MM(0.4))
    z.SetThermalReliefSpokeWidth(MM(0.45))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    o = z.Outline()
    o.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        o.Append(x, y)
    board.Add(z)

pcbnew.ZONE_FILLER(board).Fill(board.Zones())

# ---------------------------------------------------------------------------
# GND stitching vias: try a grid of candidate positions, keep the ones that
# clear all other copper and end up inside the GND pour on both layers.
# ---------------------------------------------------------------------------
VIA_D, VIA_DRILL, CLEAR, PITCH = 0.6, 0.3, 0.35, 3.0
edge_poly = pcbnew.SHAPE_POLY_SET()
board.GetBoardPolygonOutlines(edge_poly)
rule_areas = [z for z in board.Zones() if z.GetIsRuleArea()]
for fp in board.GetFootprints():
    rule_areas += [z for z in fp.Zones() if z.GetIsRuleArea()]


edge_segments = [(d.GetStart(), d.GetEnd()) for d in board.GetDrawings()
                 if d.GetLayer() == pcbnew.Edge_Cuts and d.GetShape() == pcbnew.SHAPE_T_SEGMENT]


def edge_distance(pt):
    best = None
    for a, b in edge_segments:
        ax, ay, bx, by = a.x, a.y, b.x, b.y
        dx, dy = bx - ax, by - ay
        L = dx * dx + dy * dy
        t = 0 if L == 0 else max(0.0, min(1.0, ((pt.x - ax) * dx + (pt.y - ay) * dy) / L))
        d = ((ax + t * dx - pt.x) ** 2 + (ay + t * dy - pt.y) ** 2) ** 0.5
        best = d if best is None else min(best, d)
    return best


def via_fits(pt):
    # inside the board and away from its edge
    if not edge_poly.Contains(pt):
        return False
    if edge_distance(pt) < MM(VIA_D / 2 + 0.8):
        return False
    for z in rule_areas:
        if z.GetDoNotAllowVias() and z.Outline().Collide(pt, MM(VIA_D / 2 + 0.2)):
            return False
    circle = pcbnew.SHAPE_CIRCLE(pt, MM(VIA_D / 2))
    for t in board.GetTracks():
        if t.GetEffectiveShape().Collide(circle, MM(CLEAR)):
            return False
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetEffectiveShape().Collide(circle, MM(CLEAR)):
                return False
            if p.GetDrillSize().x and p.GetEffectiveHoleShape().Collide(circle, MM(0.35)):
                return False
    for d in board.GetDrawings():          # holes drawn on Edge.Cuts
        if d.GetLayer() == pcbnew.Edge_Cuts and d.GetShape() == pcbnew.SHAPE_T_CIRCLE:
            if (d.GetCenter() - pt).EuclideanNorm() < d.GetRadius() + MM(VIA_D / 2 + 0.8):
                return False
    return True


bb = board.GetBoardEdgesBoundingBox()
added = []
y = bb.GetY() + MM(1.5)
while y < bb.GetBottom():
    x = bb.GetX() + MM(1.5)
    while x < bb.GetRight():
        pt = pcbnew.VECTOR2I(int(x), int(y))
        if via_fits(pt):
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pt)
            v.SetWidth(MM(VIA_D))
            v.SetDrill(MM(VIA_DRILL))
            v.SetNet(gnd)
            board.Add(v)
            added.append(v)
        x += MM(PITCH)
    y += MM(PITCH)

pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pours = {z.GetLayer(): z for z in board.Zones() if not z.GetIsRuleArea()}
kept = 0
for v in added:
    p = v.GetPosition()
    if all(pours[l].GetFilledPolysList(l).Contains(p) for l in (pcbnew.F_Cu, pcbnew.B_Cu)):
        kept += 1
    else:
        board.Remove(v)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
print(f"stitching vias: {kept} kept of {len(added)} candidates")

# Pour pieces cut off by traces: give each one its own via if possible.
def new_via(pt):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pt)
    v.SetWidth(MM(VIA_D))
    v.SetDrill(MM(VIA_DRILL))
    v.SetNet(gnd)
    board.Add(v)


def gnd_components():
    """Union-find over GND copper: filled pour pieces (per layer), vias,
    through-hole pads and tracks. Returns (pieces, parent-finder)."""
    pours = {z.GetLayer(): z for z in board.Zones() if not z.GetIsRuleArea()}
    pieces = []                                   # (layer, SHAPE_POLY_SET)
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        polys = pours[layer].GetFilledPolysList(layer)
        for i in range(polys.OutlineCount()):
            piece = pcbnew.SHAPE_POLY_SET(polys.Outline(i))
            for h in range(polys.HoleCount(i)):
                piece.AddHole(polys.Hole(i, h))
            pieces.append((layer, piece))
    parent = list(range(len(pieces)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i, j):
        parent[find(i)] = find(j)

    def pieces_at(pt, layers):
        return [k for k, (l, p) in enumerate(pieces) if l in layers and p.Contains(pt)]

    both = (pcbnew.F_Cu, pcbnew.B_Cu)
    links = []
    for t in board.GetTracks():
        if t.GetNetname() != "GND":
            continue
        if t.Type() == pcbnew.PCB_VIA_T:
            links.append(pieces_at(t.GetPosition(), both))
        else:
            links.append(pieces_at(t.GetStart(), (t.GetLayer(),)) +
                         pieces_at(t.GetEnd(), (t.GetLayer(),)))
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname() == "GND":
                layers = both if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else (p.GetLayer(),)
                links.append(pieces_at(p.GetPosition(), layers))
    # pads/tracks of the same footprint pad number are connected through the part
    for group in links:
        for k in group[1:]:
            union(group[0], k)
    return pieces, find


for attempt in range(6):
    pieces, find = gnd_components()
    sizes = {}
    for k, (l, p) in enumerate(pieces):
        sizes[find(k)] = sizes.get(find(k), 0) + abs(p.Area())
    main = max(sizes, key=sizes.get)
    orphans = {}
    for k in range(len(pieces)):
        if find(k) != main:
            orphans.setdefault(find(k), []).append(k)
    fixed = 0
    for comp, members in orphans.items():
        done = False
        for k in members:
            layer, piece = pieces[k]
            other = [p for j, (l, p) in enumerate(pieces) if l != layer and find(j) == main]
            bb = piece.BBox()
            yy = bb.GetY()
            while yy <= bb.GetBottom() and not done:
                xx = bb.GetX()
                while xx <= bb.GetRight():
                    pt = pcbnew.VECTOR2I(int(xx), int(yy))
                    if piece.Contains(pt) and any(o.Contains(pt) for o in other) and via_fits(pt):
                        new_via(pt)
                        fixed += 1
                        done = True
                        break
                    xx += MM(0.25)
                yy += MM(0.25)
            if done:
                break
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    print(f"connectivity pass {attempt}: {len(orphans)} detached groups, {fixed} vias added")
    if not fixed:
        break


# SMD GND pads still stranded (enclosed by signal traces): fan out with a
# short track to the nearest free via position.
def collides(shape, clearance_mm, layer):
    c = MM(clearance_mm)
    for t in board.GetTracks():
        if t.GetNetname() != "GND" and t.IsOnLayer(layer) and t.GetEffectiveShape().Collide(shape, c):
            return True
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname() != "GND" and p.IsOnLayer(layer) and p.GetEffectiveShape().Collide(shape, c):
                return True
    return False


def stranded_pads():
    pour = next(z for z in board.Zones() if not z.GetIsRuleArea() and z.GetLayer() == pcbnew.F_Cu)
    polys = pour.GetFilledPolysList(pcbnew.F_Cu)
    anchors = [v.GetPosition() for v in board.GetTracks()
               if v.Type() == pcbnew.PCB_VIA_T and v.GetNetname() == "GND"]
    anchors += [p.GetPosition() for fp in board.GetFootprints() for p in fp.Pads()
                if p.GetNetname() == "GND" and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
    out = []
    for i in range(polys.OutlineCount()):
        piece = pcbnew.SHAPE_POLY_SET(polys.Outline(i))
        if any(piece.Contains(a) for a in anchors):
            continue
        out += [p for fp in board.GetFootprints() for p in fp.Pads()
                if p.GetNetname() == "GND" and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD
                and p.IsOnLayer(pcbnew.F_Cu) and piece.Contains(p.GetPosition())]
    return out


import math
for pad in stranded_pads():
    c = pad.GetPosition()
    done = False
    for r in [x * 0.25 for x in range(4, 20)]:
        for k in range(24):
            a = k * math.pi / 12
            pt = pcbnew.VECTOR2I(int(c.x + MM(r) * math.cos(a)), int(c.y + MM(r) * math.sin(a)))
            if not via_fits(pt):
                continue
            seg = pcbnew.SHAPE_SEGMENT(c, pt, MM(0.3))
            if collides(seg, 0.2, pcbnew.F_Cu):
                continue
            helpers.add_track(board, (pcbnew.ToMM(c.x), pcbnew.ToMM(c.y)),
                              (pcbnew.ToMM(pt.x), pcbnew.ToMM(pt.y)), gnd, 0.3)
            new_via(pt)
            done = True
            break
        if done:
            break
    ref = pad.GetParentFootprint().GetReference()
    print(f"fanout {ref}.{pad.GetNumber()}: {'ok' if done else 'NO ROOM'}")
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(pcb_path, board)
print("zones filled")

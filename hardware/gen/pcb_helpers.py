"""PCB helpers shared by gen_pcb.py and finish_pcb.py."""
import sys

sys.path.insert(0, "/usr/lib/python3.8/site-packages")
import pcbnew  # noqa: E402

MM = pcbnew.FromMM


def V(x, y):
    return pcbnew.VECTOR2I(MM(x), MM(y))


def add_track(board, a, b, net, width, layer=pcbnew.F_Cu):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(V(*a))
    t.SetEnd(V(*b))
    t.SetWidth(MM(width))
    t.SetLayer(layer)
    t.SetNet(net)
    t.SetLocked(True)
    board.Add(t)


def prerouted_hdmi_ground(board, j2):
    """HDMI pads are a single 0.5mm-pitch row, so the TMDS shield pads
    (2, 5, 8, 11) and pin 17 cannot be routed between the signal traces.
    Tie them together under the connector body (the shell is GND as well)
    and to the rear shield tabs."""
    pads = {p.GetNumber(): p for p in j2.Pads()}
    pos = lambda p: (pcbnew.ToMM(p.GetPosition().x), pcbnew.ToMM(p.GetPosition().y))
    gnd = board.FindNet("GND")
    pins = [pos(pads[n]) for n in ("2", "5", "8", "11", "17")]
    bar_y = pins[0][1] - 1.9
    for x, y in pins:
        add_track(board, (x, y), (x, bar_y), gnd, 0.3)
    add_track(board, (pins[0][0], bar_y), (pins[-1][0], bar_y), gnd, 0.3)
    # rear shield tabs (the THT shield pads furthest from the board edge)
    sh = sorted((pos(p) for p in j2.Pads() if p.GetNumber() == "SH"), key=lambda q: -q[1])[:2]
    for x, y in sh:
        end = pins[0][0] if x < pins[0][0] else pins[-1][0]
        add_track(board, (end, bar_y), (x, bar_y), gnd, 0.4)
        add_track(board, (x, bar_y), (x, y), gnd, 0.4)


def gnd_fanout_points(board):
    """(pad_position, via_position) for every SMD GND pad of the ICs and
    capacitors. IC pads fan out under the package body, capacitor pads
    away from the other pad. Positions in internal units."""
    out = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref == "M1" and "Core2350B" in fp.GetFPIDAsString():
            # Core2350B: holes on a 10x10 grid, outer two rings only.
            # Corner GND pins fan out diagonally away from the module, GND pins
            # of the inner ring step one grid inwards into the empty middle.
            pads = list(fp.Pads())
            cx = sum(p.GetPosition().x for p in pads) // len(pads)
            cy = sum(p.GetPosition().y for p in pads) // len(pads)
            g = MM(2.54)
            for p in pads:
                if p.GetNetname() != "GND":
                    continue
                pos = p.GetPosition()
                c = round((pos.x - cx) / g + 4.5)
                r = round((pos.y - cy) / g + 4.5)
                sx = 1 if pos.x > cx else -1
                sy = 1 if pos.y > cy else -1
                if c in (0, 9) and r in (0, 9):
                    via = pos + pcbnew.VECTOR2I(sx * MM(1.8), sy * MM(1.8))
                elif c in (1, 8):
                    via = pos - pcbnew.VECTOR2I(sx * g, 0)
                elif r in (1, 8):
                    via = pos - pcbnew.VECTOR2I(0, sy * g)
                else:   # outer ring, not a corner
                    via = pos + (pcbnew.VECTOR2I(sx * MM(2.0), 0) if c in (0, 9)
                                 else pcbnew.VECTOR2I(0, sy * MM(2.0)))
                out.append((pos, via))
            continue
        if ref == "M1":
            # Pico GND pins: fan out towards the middle of the module so every
            # GND pin reaches the pours even when signal traces surround it
            pads = list(fp.Pads())
            centre = pcbnew.VECTOR2I(sum(p.GetPosition().x for p in pads) // len(pads),
                                     sum(p.GetPosition().y for p in pads) // len(pads))
            vertical_rows = round(fp.GetOrientationDegrees()) % 180 == 0
            for p in fp.Pads():
                if p.GetNetname() == "GND" and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                    pos = p.GetPosition()
                    if vertical_rows:
                        step = pcbnew.VECTOR2I(MM(2.2) if centre.x > pos.x else -MM(2.2), 0)
                    else:
                        step = pcbnew.VECTOR2I(0, MM(2.2) if centre.y > pos.y else -MM(2.2))
                    # inwards if the module's keepout areas allow it, else outwards
                    for via in (pos + step, pos - step):
                        mid = pcbnew.VECTOR2I((pos.x + via.x) // 2, (pos.y + via.y) // 2)
                        if not any(z.GetIsRuleArea() and (z.Outline().Collide(via, MM(0.6)) or
                                                          z.Outline().Collide(mid, MM(0.4)))
                                   for z in fp.Zones()):
                            out.append((pos, via))
                            break
            continue
        if not (ref.startswith("U") or ref.startswith("C")):
            continue
        pads = list(fp.Pads())
        centre = fp.GetPosition()
        for p in pads:
            if p.GetNetname() != "GND" or p.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
                continue
            pos = p.GetPosition()
            if ref.startswith("U"):
                # straight towards the body, perpendicular to the pad row
                d = centre - pos
                bb = p.GetBoundingBox()
                if bb.GetHeight() > bb.GetWidth():
                    d = pcbnew.VECTOR2I(0, 1 if d.y > 0 else -1)
                else:
                    d = pcbnew.VECTOR2I(1 if d.x > 0 else -1, 0)
                dist = MM(1.7) if "SOIC-20W" in fp.GetFPIDAsString() else MM(1.3)
            else:
                other = next(q for q in pads if q.GetNumber() != p.GetNumber())
                d = pos - other.GetPosition()
                dist = MM(1.35)
            n = (d.x ** 2 + d.y ** 2) ** 0.5
            via = pcbnew.VECTOR2I(int(pos.x + d.x / n * dist), int(pos.y + d.y / n * dist))
            out.append((pos, via))
    return out

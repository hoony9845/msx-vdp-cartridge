"""
Export JLCPCB fabrication files: Gerbers + drill (zipped), BOM and CPL in
JLCPCB's column format, schematic PDF.

    python3 export_fab.py <project_dir> <out_dir>
"""
import csv
import os
import shutil
import subprocess
import sys
import zipfile

import design

proj_dir, out_dir = sys.argv[1], sys.argv[2]
P = design.PROJECT
pcb = os.path.join(proj_dir, P + ".kicad_pcb")
sch = os.path.join(proj_dir, P + ".kicad_sch")
gerber_dir = os.path.join(out_dir, "gerbers")
shutil.rmtree(gerber_dir, ignore_errors=True)
os.makedirs(gerber_dir)


def run(*args):
    subprocess.run(["kicad-cli", *args], check=True, stdout=subprocess.DEVNULL)


# Gerbers (Protel extensions, soldermask subtracted from silkscreen) + drill
run("pcb", "export", "gerbers", "-o", gerber_dir + "/",
    "--layers", "F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts",
    "--subtract-soldermask", "--no-x2", "--no-netlist", pcb)
run("pcb", "export", "drill", "-o", gerber_dir + "/", "--format", "excellon",
    "--excellon-units", "mm", "--excellon-separate-th", "--generate-map",
    "--map-format", "gerberx2", pcb)

zip_path = os.path.join(out_dir, f"{P}_gerbers_jlcpcb.zip")
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(gerber_dir)):
        z.write(os.path.join(gerber_dir, name), name)

# BOM (JLCPCB columns). LCSC part numbers are left for you to fill in.
groups = {}
for ref, c in design.COMPONENTS.items():
    if ref == "J1":
        continue                        # edge connector = PCB itself
    key = (c["value"], c["footprint"], c["mpn"])
    groups.setdefault(key, []).append(ref)


def ref_key(r):
    return (r.rstrip("0123456789"), int("".join(ch for ch in r if ch.isdigit()) or 0))


with open(os.path.join(out_dir, f"{P}_bom_jlcpcb.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Comment", "Designator", "Footprint", "MPN", "LCSC Part #", "Qty", "Note"])
    for (value, fp, mpn), refs in sorted(groups.items(), key=lambda g: ref_key(sorted(g[1], key=ref_key)[0])):
        refs = sorted(refs, key=ref_key)
        note = ""
        if refs == ["M1"]:
            note = "Hand-mounted module on 2x20 pin headers - not assembled by JLCPCB"
        elif refs == ["J2"]:
            note = "Check that the purchased HDMI receptacle matches this footprint"
        w.writerow([value, ",".join(refs), fp.split(":")[1], mpn, "", len(refs), note])

# CPL (pick and place) for SMD parts, JLCPCB column names
pos_tmp = os.path.join(out_dir, "_pos.csv")
run("pcb", "export", "pos", "-o", pos_tmp, "--format", "csv", "--units", "mm",
    "--side", "both", "--smd-only", "--exclude-fp-th", pcb)
with open(pos_tmp) as fi, open(os.path.join(out_dir, f"{P}_cpl_jlcpcb.csv"), "w", newline="") as fo:
    r = csv.DictReader(fi)
    w = csv.writer(fo)
    w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for row in r:
        w.writerow([row["Ref"], row["PosX"] + "mm", row["PosY"] + "mm",
                    "Top" if row["Side"] == "top" else "Bottom", row["Rot"]])
os.remove(pos_tmp)

run("sch", "export", "pdf", "-o", os.path.join(out_dir, f"{P}_schematic.pdf"), sch)
print("fabrication files in", out_dir)
for name in sorted(os.listdir(out_dir)):
    print("  ", name)

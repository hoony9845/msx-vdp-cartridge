"""
Autoroute the board with Freerouting (Specctra DSN/SES round trip).

    python3 route.py <project_dir> [passes]
"""
import glob
import os
import subprocess
import sys

sys.path.insert(0, "/usr/lib/python3.8/site-packages")
import pcbnew  # noqa: E402

import design  # noqa: E402

proj_dir = sys.argv[1]
passes = sys.argv[2] if len(sys.argv) > 2 else "100"
pcb_path = os.path.join(proj_dir, design.PROJECT + ".kicad_pcb")
dsn = os.path.join(proj_dir, "route.dsn")
ses = os.path.join(proj_dir, "route.ses")

FREEROUTING = os.path.expanduser("~/tools/freerouting/freerouting-2.4.1.jar")
JAVA = (glob.glob(os.path.expanduser("~/tools/jre25/*/bin/java")) or ["java"])[0]

board = pcbnew.LoadBoard(pcb_path)
if not pcbnew.ExportSpecctraDSN(board, dsn):
    raise SystemExit("DSN export failed")

# Freerouting rounds clearances slightly below the KiCad rule (0.197 < 0.2mm);
# route with a 10% margin so the result passes KiCad DRC.
text = open(dsn).read()
text = text.replace("(clearance 200)", "(clearance 220)")

# GND is connected by the copper pours on both layers (finish_pcb.py), so
# leave it out of autorouting: its pads become plain obstacles.
import re
from sexp import find_block
start = text.find("(net GND\n")
if start < 0:
    start = text.find("(net GND ")
if start >= 0:
    block = find_block(text[start:], text[start:start + 9])
    text = text[:start] + text[start + len(block):]
text = re.sub(r"\(class Power \+3V3 \+5V GND", "(class Power +3V3 +5V", text)
open(dsn, "w").write(text)

# Freerouting results vary with the strategy; try several until one routes
# every connection (the FR_ARGS environment variable is tried first).
strategies = [os.environ.get("FR_ARGS", "-us hybrid -hr 1:1"), "-us hybrid -hr 2:1",
              "-is random", "-us greedy", "-us global", "-us hybrid -hr 1:2"]
best = None
rounds = int(os.environ.get("FR_ROUNDS", "4"))
for args in strategies * rounds:
    res = subprocess.run([JAVA, "-jar", FREEROUTING, "-de", dsn, "-do", ses, "-mp", passes,
                          "--gui.enabled=false", *args.split()],
                         capture_output=True, text=True)
    log = res.stdout + res.stderr
    unrouted = [l.strip() for l in log.splitlines() if l.startswith("  Net '")]
    print(f"strategy '{args}': {len(unrouted)} unrouted {unrouted}")
    if res.returncode == 0 and (best is None or len(unrouted) < best[0]):
        best = (len(unrouted), args)
        os.replace(ses, ses + ".best")
    if best and best[0] == 0:
        break
if best is None:
    raise SystemExit("Freerouting failed")
os.replace(ses + ".best", ses)
print("using strategy:", best[1], "unrouted:", best[0])
if best[0]:
    print("WARNING: autorouting incomplete - DRC will report the missing connections")

board = pcbnew.LoadBoard(pcb_path)
if not pcbnew.ImportSpecctraSES(board, ses):
    raise SystemExit("SES import failed")
pcbnew.SaveBoard(pcb_path, board)
print("routed:", len(board.GetTracks()), "track/via items")

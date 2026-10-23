"""
Add neighbouring-frame shots to a recorded trace: for every "shot X" also
render X__o<k> for k = -4..+2 frames around it. openMSX's screenshot can
show the picture of a few frames earlier than the trace position, so
compare.py --window accepts the best candidate.

    python3 shot_window.py <trace.txt> <out.txt>
"""
import sys

OFFSETS = range(-4, 3)

lines = open(sys.argv[1]).read().split("\n")
shots, n = {}, 0
for line in lines:
    if line == "frame":
        n += 1
    elif line.startswith("shot "):
        shots.setdefault(n, []).append(line.split()[1])

want = {}
for frame, names in shots.items():
    for name in names:
        for off in OFFSETS:
            want.setdefault(frame + off, []).append(f"{name}__o{off}")

out, n = [], 0
out += [f"shot {s}" for s in want.get(0, [])]
for line in lines:
    if line.startswith("shot "):
        continue
    out.append(line)
    if line == "frame":
        n += 1
        out += [f"shot {s}" for s in want.get(n, [])]
open(sys.argv[2], "w").write("\n".join(out) + "\n")

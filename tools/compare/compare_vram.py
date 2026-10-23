"""
Compare our VRAM with openMSX's at every recorded shot (record with
V9990_DUMP_VRAM=1, replay with REPLAY_DUMP_VRAM=1). Separates "the core
computed different pixels" from "the picture was scanned at another time".

Bytes the software never wrote keep openMSX's power-on pattern while ours are
0; they are ignored using the dump of the first shot (taken before the
software touched the V9990, e.g. f000060).

    python3 compare_vram.py <record_dir> <ours_dir> [first_frame] [bytes_per_line]
"""
import glob
import os
import sys

rec, ours = sys.argv[1], sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
pitch = int(sys.argv[4]) if len(sys.argv) > 4 else 512
dumps = sorted(glob.glob(os.path.join(rec, "f*.vram")))
power_on = open(dumps[0], "rb").read()


def linear(p):              # physical (openMSX/ours layout) -> bitmap CPU address
    return ((p & 0x3FFFF) << 1) | (p >> 18)


equal, lines = 0, []
for path in dumps:
    name = os.path.basename(path)[:-5]
    if int(name[1:]) < first:
        continue
    a = open(os.path.join(ours, name + ".vram"), "rb").read()
    b = open(path, "rb").read()
    diff = [i for i in range(len(a)) if a[i] != b[i] and not (a[i] == 0 and b[i] == power_on[i])]
    if not diff:
        equal += 1
        continue
    ys = sorted({linear(p) // pitch for p in diff})
    lines.append(f"| {name} | {len(diff)} | {ys[0]}-{ys[-1]} ({len(ys)} lines) |")
print(f"VRAM identical at {equal} of {equal + len(lines)} shots")
if lines:
    print("| shot | differing bytes | image lines |\n|---|---|---|\n" + "\n".join(lines))

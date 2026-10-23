"""
Compare screenshots of our core (host/replay, .ppm) with openMSX (.png).

The V9990 display area is located in each image with the "calibration"
shot (a full-screen single-colour P1 picture). Every shot is then sampled
at the centre of each V9990 dot (resolution from the <shot>.size file
written by host/replay) and compared dot by dot.

    python3 compare.py <ours_dir> <openmsx_dir> <report_dir>
"""
import os
import sys

from PIL import Image, ImageChops

TOLERANCE = 12          # per channel, absorbs 5-bit -> 8-bit rounding differences


def load(path):
    return Image.open(path).convert("RGB")


def display_rect(img):
    """Bounding box of the non-black area of the calibration shot."""
    bbox = Image.eval(img.convert("L"), lambda v: 255 if v > 64 else 0).getbbox()
    if not bbox:
        raise SystemExit("calibration shot is empty")
    return bbox


def dot_columns(img, rect, src_w):
    """Pixel column of the centre of every dot, measured on a calib_dots
    shot (white/black alternating dots): each run of equal colour is a dot."""
    left, top, right, bottom = rect
    px = img.load()
    y = (top + bottom) // 2
    runs, start = [], left
    lit = lambda x: sum(px[x, y]) > 300
    for x in range(left + 1, right + 1):
        if x == right or lit(x) != lit(x - 1):
            runs.append((start + x - 1) // 2)
            start = x
    if len(runs) != src_w:
        print(f"warning: found {len(runs)} dots instead of {src_w}, using uniform spacing")
        cw = (right - left) / src_w
        return [int(left + (i + 0.5) * cw) for i in range(src_w)]
    return runs


def sample(img, rect, src_w, src_h, columns):
    """Take the centre of every V9990 dot inside the display rectangle,
    giving a src_w x src_h image independent of how it was scaled."""
    left, top, right, bottom = rect
    ch = (bottom - top) / src_h
    out = Image.new("RGB", (src_w, src_h))
    px = img.load()
    po = out.load()
    for y in range(src_h):
        sy = int(top + (y + 0.5) * ch)
        for x in range(src_w):
            po[x, y] = px[columns[x], sy]
    return out


def compare(a, b):
    """Return (fraction of differing dots, mask of differing dots)."""
    diff = ImageChops.difference(a, b)
    r, g, bl = Image.eval(diff, lambda v: 255 if v > TOLERANCE else 0).split()
    bad = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    n_bad = sum(1 for v in bad.getdata() if v)
    return n_bad / (a.size[0] * a.size[1]), bad


def main():
    ours_dir, omsx_dir, report_dir = sys.argv[1:4]
    os.makedirs(report_dir, exist_ok=True)

    rect_ours = display_rect(load(os.path.join(ours_dir, "calibration.ppm")))
    rect_omsx = display_rect(load(os.path.join(omsx_dir, "calibration.png")))
    lines = ["# V9990 core vs openMSX", "",
             f"display area: ours {rect_ours}, openMSX {rect_omsx}", ""]
    lines.append("Both shots are sampled at the centre of every V9990 dot, located with "
                 "the calib_dots shots (openMSX stretches the picture horizontally).")
    lines.append("")

    columns = {}
    for w in (256, 512):
        name = f"calib_dots_{w}"
        columns[("ours", w)] = dot_columns(load(os.path.join(ours_dir, name + ".ppm")), rect_ours, w)
        columns[("omsx", w)] = dot_columns(load(os.path.join(omsx_dir, name + ".png")), rect_omsx, w)

    lines += ["| shot | V9990 dots | differing dots | result |", "|---|---|---|---|"]
    worst = 0.0
    for name in sorted(os.listdir(omsx_dir)):
        if not name.endswith(".png"):
            continue
        shot = name[:-4]
        ours_path = os.path.join(ours_dir, shot + ".ppm")
        if not os.path.exists(ours_path):
            lines.append(f"| {shot} | – | – | missing in ours |")
            continue
        with open(os.path.join(ours_dir, shot + ".size")) as f:
            src_w, src_h = (int(v) for v in f.read().split())
        if ("ours", src_w) not in columns:
            lines.append(f"| {shot} | {src_w}x{src_h} | – | no dot calibration for this width |")
            continue
        a = sample(load(ours_path), rect_ours, src_w, src_h, columns[("ours", src_w)])
        b = sample(load(os.path.join(omsx_dir, name)), rect_omsx, src_w, src_h, columns[("omsx", src_w)])
        frac, bad = compare(a, b)
        worst = max(worst, frac)
        vis = Image.blend(a.convert("L").convert("RGB"), Image.new("RGB", a.size), 0.6)
        vis.paste((255, 0, 0), mask=bad)
        side = Image.new("RGB", (src_w * 3, src_h))
        for i, im in enumerate((a, b, vis)):
            side.paste(im, (src_w * i, 0))
        side = side.resize((side.size[0] * 2, side.size[1] * 2), Image.NEAREST)
        side.save(os.path.join(report_dir, shot + "_ours_openmsx_diff.png"))
        verdict = "OK" if frac == 0 else ("close" if frac < 0.001 else "DIFFERENT")
        lines.append(f"| {shot} | {src_w}x{src_h} | {frac * 100:.3f}% | {verdict} |")

    with open(os.path.join(report_dir, "report.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if worst < 0.001 else 1


if __name__ == "__main__":
    sys.exit(main())

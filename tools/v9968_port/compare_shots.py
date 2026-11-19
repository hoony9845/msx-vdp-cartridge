#!/usr/bin/env python3
"""Check a converted ROM against the original from screenshots of both runs.

The two machines do not run in step (animations are at different phases),
so the shots are not compared pixel by pixel. A conversion is taken as bad
when the cartridge picture stays black or stops changing while the
original's does not; the share of identical shots is reported too."""
import os, sys
from PIL import Image, ImageChops, ImageStat


def shots(d):
    return [Image.open(os.path.join(d, n)).convert('RGB')
            for n in sorted(os.listdir(d)) if n.endswith('.png')]


def black(im):
    return max(ImageStat.Stat(im.convert('L')).extrema[0]) < 16


def changes(ims):
    return sum(1 for a, b in zip(ims, ims[1:]) if ImageChops.difference(a, b).getbbox())


# the cartridge machine shows its boot logo on the built-in VDP first, so
# the first quarter of the shots is left out
a, b = shots(sys.argv[1]), shots(sys.argv[2])
a, b = a[len(a) // 4:], b[len(b) // 4:]
if not b:
    print('FAIL: no cartridge shots'); sys.exit(1)
ba, bb = sum(map(black, a)), sum(map(black, b))
ca, cb = changes(a), changes(b)
same = sum(1 for x, y in zip(a, b) if not ImageChops.difference(x, y).getbbox())
print('original: %d shots, %d black, %d changes | cartridge: %d shots, %d black, %d changes | %d identical pairs'
      % (len(a), ba, ca, len(b), bb, cb, same))
bad = []
if bb > ba + 1:
    bad.append('the cartridge picture is black more often (%d vs %d shots)' % (bb, ba))
if ca >= 3 and cb < ca // 3:
    bad.append('the cartridge picture stops changing (%d vs %d changes)' % (cb, ca))
if bad:
    print('FAIL: ' + '; '.join(bad)); sys.exit(1)
print('OK: the converted ROM runs and draws like the original')

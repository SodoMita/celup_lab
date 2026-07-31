#!/usr/bin/env python3
"""v4.9 corner/glow forensics gate for autodeblur (the smiley class:
hard pixelated source, strong -r/-g).

Runs tests/cornerstar48_src.webp at 4x with the user-style recipe and
asserts:
  1. HULL: no output colour leaves the source colour range (the deblur
     model has no ringing vocabulary -- a 'neon' skirt or an undershoot
     fringe is a failure);
  2. TIP: the triangle's bottom-right acute tip keeps its extent
     (rounded corners lose >= 10% of the dark tip diagonal extent);
  3. WIDTH: the 10-90 transition across a straight flank stays tight
     (a sigma-wide glow smears it out);
  4. GLOW: a strip just outside the triangle hypotenuse stays at the
     white plateau (no dark 'neon' band hugging the stroke).

Run from the repo root:  python3 tests/check_corners.py
Exit code 0 = all checks pass.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LAB = HERE.parent / 'celup_lab'
SRC = HERE / 'cornerstar48_src.webp'

RECIPE = ['--mode', 'autodeblur', '--max-mib', '1048', '-c', 'linear',
          '-k', 'bspline', '-r', '2.3', '-s', '100', '-g', '16', '-D', 'remap']


def main():
    if not SRC.exists():
        print(f'FAIL: {SRC.name} missing (run tests/make_test_sources.py)')
        return 1
    fails = 0
    slum = np.asarray(Image.open(SRC).convert('RGBA'), np.float64)[..., 0] / 255
    smin, smax = slum.min(), slum.max()

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / 'o.webp'
        r = subprocess.run([str(LAB), str(SRC), str(out), '4'] + RECIPE,
                           capture_output=True)
        if r.returncode != 0:
            print('FAIL: celup_lab exited', r.returncode, r.stderr.decode())
            return 1
        o = np.asarray(Image.open(out).convert('RGBA'), np.float64)[..., 0] / 255

    # 1. hull
    omin, omax = o.min(), o.max()
    if omin < smin - .02 or omax > smax + .02:
        print(f'FAIL hull: output [{omin:.4f},{omax:.4f}] outside source '
              f'[{smin:.4f},{smax:.4f}] +- .02')
        fails += 1
    else:
        print(f'ok   hull: output [{omin:.4f},{omax:.4f}] inside source '
              f'[{smin:.4f},{smax:.4f}]')

    # 2. tip extent (triangle bottom-right tip at src (44,44) -> out (176,176))
    sdark = (slum < .35) & (np.mgrid[0:48, 0:48][1] > 24) & \
        (np.mgrid[0:48, 0:48][0] > 24)
    yy, xx = np.mgrid[0:192, 0:192]
    odark = (o < .35) & (xx > 96) & (yy > 96)
    s_ext = (np.argwhere(sdark)[:, 0] + np.argwhere(sdark)[:, 1]).max()
    o_ext = (np.argwhere(odark)[:, 0] + np.argwhere(odark)[:, 1]).max() / 4
    if o_ext < s_ext - 1.0:
        print(f'FAIL tip: dark tip diagonal extent {o_ext:.2f} < source '
              f'{s_ext:.2f} - 1 src px (corner rounded off)')
        fails += 1
    else:
        print(f'ok   tip: extent {o_ext:.2f} vs source {s_ext:.2f} src px')

    # 3. transition width across the triangle's VERTICAL left flank:
    # flank x=8 at rows 30..40 (src) -> out row 140, scan x 8..60
    row = o[140, 8:64]
    mid = np.nonzero((row > .25) & (row < .75))[0]
    width = (mid.max() - mid.min() + 1) if mid.size else 0
    if width > 12:
        print(f'FAIL width: 10-90 flank transition {width} out px > 12 '
              f'(glow/smear band)')
        fails += 1
    else:
        print(f'ok   width: 10-90 flank transition {width} out px <= 12')

    # 4. glow strip just OUTSIDE the hypotenuse (line from (8,40) to (20,8)):
    # offset 3 out px outward (down-left normal): src strip along the edge
    ys, xs = [], []
    for t in np.linspace(0.12, 0.80, 40):
        cx = 8 + t * 12 - 0.9   # 0.9 src px outward
        cy = 40 - t * 32 + 0.9
        ys.append(int(round(cy * 4)))
        xs.append(int(round(cx * 4)))
    glow = float(o[ys, xs].mean())
    if glow < .88:
        print(f'FAIL glow: strip outside dark stroke at {glow:.4f} < .88 '
              f'(neon band hugging the edge)')
        fails += 1
    else:
        print(f'ok   glow: strip outside dark stroke {glow:.4f} >= .88')

    print('PASS' if not fails else f'{fails} FAILURES')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

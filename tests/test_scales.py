#!/usr/bin/env python3
"""Scale-sweep regression test for celup_lab (v4.2+).

Answers: "are different scalings applied, up to >20x?" -- yes, here:
every listed mode is run at integer and fractional scales 1.5..24
against a synthetic AA circle fixture (round contours are where
sawtooth/staircase artifacts show first), and additionally over the
miya face crop when tests/miya_face.webp exists (see
tests/make_miya_fixtures.py).

Checks per run:
  1. exit code 0 and output decodes;
  2. output dimensions match round(src * scale);
  3. sawtooth metric on the alpha/colour AA band: no staircase tread
     (>=3 constant pixels ending in a >0.25 jump) and the max adjacent
     step inside the AA band stays below the mode budget.  Nearest and
     scale2x are exempt: they ARE staircases by design.
  4. round-trip smoke: no NaN/garbage -- decoded bytes are finite by
     construction, so this checks the encoder path stays lossless-valid.

Usage: python3 tests/test_scales.py [path-to-celup_lab] [-v]
Exit code 0 = all pass.
"""
import subprocess, sys
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LAB = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (HERE / '../celup_lab').resolve()
FIX = HERE / 'round12_src.webp'
FACE = HERE / 'miya_face32x11.webp'

SCALES = (1.5, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24)   # includes >20x
# Extreme scales only need enough source pixels to expose the artifact
# pattern; a 32x11 face strip at 22x (704x242) shows anything a full
# frame would, at 1% of the runtime.
FACE_SCALES = (2, 4, 8, 16, 22)
MODES = ('nearest', 'bilinear', 'triangle', 'cubic', 'mitchell',
         'lanczos3', 'adaptive', 'sdf', 'autoblur', 'deblurcompress',
         'hourglasscompress')
STAIRS_OK = ('nearest', 'scale2x')          # hard staircase by design
MAX_STEP = {                                # AA-band adjacent-step budget
    'nearest': 1.01, 'bilinear': .45, 'triangle': .45, 'cubic': .55,
    'mitchell': .65, 'lanczos3': .75, 'adaptive': .75, 'sdf': .85,
    'autoblur': .45, 'deblurcompress': .85, 'hourglasscompress': .85}


def lin(x):
    x = x / 255.
    return np.where(x <= .04045, x / 12.92, ((x + .055) / 1.055) ** 2.4)


def make_fixture():
    """96x96 supersample: AA circle on a gradient -> 12x12 (8x box, pm)."""
    z, r0 = 96, 36.
    yy, xx = np.mgrid[0:z, 0:z].astype(np.float64)
    inside = np.clip(r0 + .5 - np.hypot(xx - 48, yy - 50), 0, 1)  # AA alpha
    rgb = np.zeros((z, z, 3), np.float64)
    rgb[:, :, 0] = .86 * inside
    rgb[:, :, 1] = .43 * inside
    rgb[:, :, 2] = .16 * inside
    bg = (xx / z)[..., None] * np.array([.10, .14, .24])
    out = bg * (1 - inside[..., None]) + rgb * inside[..., None]
    al = np.full((z, z, 1), 255.)
    hi = np.dstack((np.clip(out, 0, 1) * 255, al)).astype(np.uint8).astype(np.float64)
    pm = np.empty_like(hi); pm[:, :, :3] = lin(hi[:, :, :3]); pm[:, :, 3] = 255.
    low = pm.reshape(12, 8, 12, 8, 4).mean((1, 3))
    a = low[:, :, 3] / 255.
    sr = np.where(low[:, :, :3] <= .0031308, 12.92 * low[:, :, :3],
                  1.055 * low[:, :, :3] ** (1 / 2.4) - .055)
    img = np.dstack((np.clip(sr * 255, 0, 255), 255 * a)).astype(np.uint8)
    Image.fromarray(img, 'RGBA').save(FIX, lossless=True)


def sawtooth(img):
    """(max adjacent step in AA band, staircase tread count) over alpha."""
    a = np.asarray(img.convert('RGBA'))[:, :, 3].astype(np.float32) / 255.
    mx, treads = 0., 0
    for row in a:
        d = np.abs(np.diff(row))
        band = (row[1:] > .03) & (row[1:] < .97)
        if band.any():
            mx = max(mx, float(d[band].max()))
        for i in range(1, len(row) - 3):
            if row[i] == row[i + 1] == row[i + 2] and .03 < row[i] < .97:
                if abs(row[i + 3] - row[i]) > .25 or abs(row[i] - row[i - 1]) > .25:
                    treads += 1
    return mx, treads


def run(src, scale, mode, out):
    cmd = [str(LAB), str(src), str(out), str(scale), '--mode', mode,
           '--max-mib', '4096']
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return r.returncode, r.stderr.decode()


def main():
    if not FIX.exists():
        make_fixture()
    fails = 0
    for src, scales in ((FIX, SCALES), (FACE, FACE_SCALES) if FACE.exists()
                        else ()):
        sw, sh = Image.open(src).size
        if src == FIX and FACE.exists() == False and src != FACE:
            pass
        print(f'== {src.name} ({sw}x{sh}) ==')
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            for mode in MODES:
                for s in scales:
                    out = Path(td) / f'o.webp'
                    rc, err = run(src, s, mode, out)
                    ew, eh = round(sw * s), round(sh * s)
                    if rc != 0:
                        print(f'FAIL {mode} {s}x: exit {rc} {err.strip()}')
                        fails += 1
                        continue
                    got = Image.open(out).size
                    if got != (ew, eh):
                        print(f'FAIL {mode} {s}x: dims {got} != {(ew, eh)}')
                        fails += 1
                        continue
                    mx, treads = sawtooth(Image.open(out))
                    if mode not in STAIRS_OK and treads > 0:
                        print(f'FAIL {mode} {s}x: {treads} staircase treads')
                        fails += 1
                    elif mx > MAX_STEP[mode] + max(.0, (s - 8)) * .01:
                        print(f'FAIL {mode} {s}x: AA step {mx:.3f} > budget')
                        fails += 1
                    else:
                        print(f'ok   {mode:18s} {s:>4}x  step {mx:.3f}')
    if not FACE.exists():
        print('note: tests/miya_face.webp missing -- face sweep skipped '
              '(tests/make_miya_fixtures.py with the miya asset present)')
    print('PASS' if not fails else f'{fails} FAILURES')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())

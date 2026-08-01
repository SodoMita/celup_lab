#!/usr/bin/env python3
"""Artifact-focused evaluation for celup_lab modes.

For each candidate and scene:
  MAE  - premultiplied-linear RGBA mean absolute error vs ground truth
  HG   - hourglass amplitude: RMS of per-cell fitted coefficients of the
         hourglass/saddle bases b0 = |u-.5|-|v-.5| and b1 = (u-.5)(v-.5)
         on the residual (hr - bilinear reference), in pm-linear luma.
         This directly measures the bow-tie/checker artifact energy that
         MAE cannot see (an hourglass error can have tiny MAE impact).
  CHK  - MAE restricted to high-checker-confidence source cells.

Usage: python3 hourglass_metric.py ./celup_lab:adaptive ./celup_lab_baseline:deblurcompress ...
"""
import subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

S, N = 4, 96


def candidate(spec):
    prefix = spec.lower().split(':', 1)[0]
    if prefix in ('pil', 'cv2', 'scipy', 'py'):
        return (prefix, spec.split(':', 1)[1].lower() if ':' in spec else 'default', [])
    p = Path(spec); mode = None
    if ':' in spec:
        left, right = spec.rsplit(':', 1)
        if left and right:
            p = Path(left); mode = right
    extra = []
    if mode and ' ' in mode:
        parts = mode.split()
        mode, extra = parts[0], parts[1:]
    return (p.resolve(), mode, extra)


def cname(c):
    exe, mode, extra = c
    if isinstance(exe, str):
        return f"{exe}:{mode}"
    return exe.name + ((':' + mode) if mode else '') + ((' ' + ' '.join(extra)) if extra else '')


def lin(x):
    x = x / 255.
    return np.where(x <= .04045, x / 12.92, ((x + .055) / 1.055) ** 2.4)


def pm(a):
    a = np.asarray(a, dtype=np.float32)
    o = np.empty_like(a)
    o[:, :, :3] = lin(a[:, :, :3]) * (a[:, :, 3:4] / 255.)
    o[:, :, 3] = a[:, :, 3] / 255.
    return o


def encode_pm(p):
    a = np.clip(p[:, :, 3:4], 0, 1)
    rgb = np.divide(p[:, :, :3], np.maximum(a, 1e-8))
    sr = np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - .055)
    rgba = np.dstack((np.clip(sr * 255, 0, 255), a[:, :, 0] * 255)).astype(np.uint8)
    return Image.fromarray(rgba, 'RGBA')


def down(a):
    return a.reshape(N, S, N, S, 4).mean((1, 3))


def scene(name):
    z = N * S
    yy, xx = np.mgrid[0:z, 0:z]
    if name == 'checker1':
        # 1 source px checker (Nyquist): 4x4 truth px blocks
        c = ((xx // S + yy // S) & 1)
        a = np.zeros((z, z, 4), np.uint8)
        a[:, :, 0] = np.where(c, 28, 238); a[:, :, 1] = np.where(c, 88, 218)
        a[:, :, 2] = np.where(c, 28, 238); a[:, :, 3] = 255
        return pm(a)
    if name == 'checker2':
        c = ((xx // (2 * S) + yy // (2 * S)) & 1)
        a = np.zeros((z, z, 4), np.uint8)
        a[:, :, 0] = np.where(c, 40, 225); a[:, :, 1] = np.where(c, 190, 60)
        a[:, :, 2] = np.where(c, 60, 200); a[:, :, 3] = 255
        return pm(a)
    if name == 'crosshatch':
        a = np.zeros((z, z, 4), np.uint8); a[:] = (250, 250, 252, 255)
        xh = (((xx - yy) % (8 * S)) < S) | (((xx + yy) % (8 * S)) < S)
        a[xh] = (25, 25, 130, 255)
        return pm(a)
    if name == 'rings':
        r = np.sqrt((xx - z / 2) ** 2 + (yy - z / 2) ** 2)
        a = np.zeros((z, z, 4), np.uint8); a[:] = (24, 30, 44, 255)
        a[(r % (6 * S)) < (2 * S)] = (245, 200, 70, 255)
        return pm(a)
    if name == 'diag':
        im = Image.new('RGBA', (z, z), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
        d.polygon([(0, z), (0, 3 * z // 4), (z, z // 4), (z, z // 2)], fill=(20, 210, 255, 255))
        d.line((0, 3 * z // 4, z, z // 4), fill='white', width=2 * S)
        return pm(np.asarray(im))
    if name == 'corner':
        im = Image.new('RGBA', (z, z), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
        d.polygon([(z//8, z//8), (7*z//8, z//8), (7*z//8, 7*z//8), (z//2, 5*z//8), (z//8, 7*z//8)],
                  fill=(180, 60, 180, 255))
        d.line([(z//8, z//8), (7*z//8, z//8), (7*z//8, 7*z//8), (z//2, 5*z//8), (z//8, 7*z//8), (z//8, z//8)],
               fill=(255, 255, 255, 255), width=S)
        return pm(np.asarray(im))
    raise SystemExit(f'unknown scene {name}')


SCENES = ('checker1', 'checker2', 'crosshatch', 'rings', 'diag', 'corner')


def bilinear_ref(low_pm):
    """Bilinear x4 reconstruction of low_pm at the truth resolution (numpy)."""
    H, W = low_pm.shape[:2]
    out = np.zeros((H * S, W * S, 4), np.float32)
    for oy in range(H * S):
        sy = (oy + .5) / S - .5
        iy = int(np.floor(sy)); fy = sy - iy
        y0 = min(max(iy, 0), H - 1); y1 = min(max(iy + 1, 0), H - 1)
        for ox in range(W * S):
            sx = (ox + .5) / S - .5
            ix = int(np.floor(sx)); fx = sx - ix
            x0 = min(max(ix, 0), W - 1); x1 = min(max(ix + 1, 0), W - 1)
            out[oy, ox] = ((1 - fx) * (1 - fy) * low_pm[y0, x0] + fx * (1 - fy) * low_pm[y0, x1] +
                           (1 - fx) * fy * low_pm[y1, x0] + fx * fy * low_pm[y1, x1])
    return out


def luma(p):
    return .2126 * p[:, :, 0] + .7152 * p[:, :, 1] + .0722 * p[:, :, 2] + .35 * p[:, :, 3]


def hourglass_amp(hr_pm, low_pm):
    """Fit b0,b1 per cell on residual luma; return RMS amplitude over cells."""
    ref = bilinear_ref(low_pm)
    r = luma(hr_pm) - luma(ref)
    rc = r.reshape(N, S, N, S)
    u = (np.arange(S) + .5) / S
    b0 = np.abs(u - .5)[None, :] - np.abs(u - .5)[:, None]   # [sy,sx]
    b1 = ((u - .5)[None, :]) * ((u - .5)[:, None])
    b0c = b0 - b0.mean(); b1c = b1 - b1.mean()
    g00 = (b0c * b0c).sum(); g11 = (b1c * b1c).sum(); g01 = (b0c * b1c).sum()
    det = g00 * g11 - g01 * g01
    a0 = (rc * b0c[None, :, None, :]).sum((1, 3))
    a1 = (rc * b1c[None, :, None, :]).sum((1, 3))
    c0 = (a0 * g11 - a1 * g01) / det
    c1 = (a1 * g00 - a0 * g01) / det
    amp = np.sqrt(np.maximum(c0 ** 2 * g00 + c1 ** 2 * g11 + 2 * c0 * c1 * g01, 0) / (S * S))
    return float(np.sqrt((amp ** 2).mean())), float(np.percentile(amp, 95))


def checker_cells(low_pm):
    """Mask of source cells that look like 2x2 checkers (for CHK metric)."""
    H, W = low_pm.shape[:2]
    m = np.zeros((H, W), bool)
    d = low_pm
    for y in range(H - 1):
        for x in range(W - 1):
            cell = d[y:y + 2, x:x + 2].reshape(4, 4)
            d03 = ((cell[0] - cell[3]) ** 2).sum(); d12 = ((cell[1] - cell[2]) ** 2).sum()
            cross = .25 * (((cell[0] - cell[1]) ** 2).sum() + ((cell[0] - cell[2]) ** 2).sum() +
                           ((cell[3] - cell[1]) ** 2).sum() + ((cell[3] - cell[2]) ** 2).sum())
            if cross > 4e-4 and (d03 + d12) / (cross + 1e-20) < .05:
                m[y, x] = True
    return m


def run_candidate(c, inp, out):
    exe, mode, extra = c
    if isinstance(exe, str):
        im = Image.open(inp)
        w, h = im.size
        dw, dh = w * 4, h * 4
        if exe == 'pil':
            res = {'nearest': Image.Resampling.NEAREST, 'bilinear': Image.Resampling.BILINEAR,
                   'bicubic': Image.Resampling.BICUBIC, 'lanczos': Image.Resampling.LANCZOS}.get(mode, Image.Resampling.BICUBIC)
            im.resize((dw, dh), res).save(out)
        elif exe == 'cv2':
            import cv2
            arr = np.asarray(im)
            inter = {'nearest': cv2.INTER_NEAREST, 'bilinear': cv2.INTER_LINEAR,
                     'cubic': cv2.INTER_CUBIC, 'lanczos4': cv2.INTER_LANCZOS4}.get(mode, cv2.INTER_CUBIC)
            Image.fromarray(cv2.resize(arr, (dw, dh), interpolation=inter), im.mode).save(out)
        elif exe == 'scipy':
            from scipy.ndimage import zoom
            arr = np.asarray(im, dtype=np.float32)
            order = 5 if mode == 'spline5' else (3 if mode == 'spline3' else 1)
            Image.fromarray(np.clip(zoom(arr, (4, 4, 1), order=order), 0, 255).astype(np.uint8), im.mode).save(out)
        elif exe == 'py':
            import cv2
            arr = np.asarray(im)
            base = cv2.resize(arr, (dw, dh), interpolation=cv2.INTER_LANCZOS4).astype(np.float32)
            if mode == 'edgedir':
                lum = base[:, :, :3].mean(axis=2)
                gx = cv2.Sobel(lum, cv2.CV_32F, 1, 0, ksize=3); gy = cv2.Sobel(lum, cv2.CV_32F, 0, 1, ksize=3)
                mag = np.sqrt(gx**2 + gy**2) + 1e-6
                nx = gx / mag; ny = gy / mag
                yy, xx = np.mgrid[0:dh, 0:dw]
                xx_push = np.clip(xx - nx * 0.8, 0, dw - 1).astype(np.float32)
                yy_push = np.clip(yy - ny * 0.8, 0, dh - 1).astype(np.float32)
                pushed = np.empty_like(base)
                for ch in range(base.shape[2]):
                    pushed[:, :, ch] = cv2.remap(base[:, :, ch], xx_push, yy_push, cv2.INTER_LINEAR)
                wt = np.clip((mag - 10.0) / 40.0, 0.0, 0.7)[:, :, None]
                base = base * (1.0 - wt) + pushed * wt
            elif mode == 'vector':
                base = cv2.resize(np.asarray(im), (dw, dh), interpolation=cv2.INTER_CUBIC).astype(np.float32)
                lum = base[:, :, :3].mean(axis=2)
                gx = cv2.Sobel(lum, cv2.CV_32F, 1, 0, ksize=3); gy = cv2.Sobel(lum, cv2.CV_32F, 0, 1, ksize=3)
                mag = np.sqrt(gx**2 + gy**2) + 1e-6
                nx = gx / mag; ny = gy / mag
                tx = -ny; ty = nx
                yy, xx = np.mgrid[0:dh, 0:dw]
                acc = base.copy()
                w_tot = np.ones((dh, dw, 1), dtype=np.float32)
                for step in [-2.0, -1.0, 1.0, 2.0]:
                    wt = np.exp(-0.5 * (step / 1.5)**2)
                    xs = np.clip(xx + tx * step, 0, dw - 1).astype(np.float32)
                    ys = np.clip(yy + ty * step, 0, dh - 1).astype(np.float32)
                    for ch in range(base.shape[2]):
                        acc[:, :, ch] += wt * cv2.remap(base[:, :, ch], xs, ys, cv2.INTER_LINEAR)
                    w_tot += wt
                smoothed = acc / w_tot
                shift = 0.6
                xx_push = np.clip(xx - nx * shift, 0, dw - 1).astype(np.float32)
                yy_push = np.clip(yy - ny * shift, 0, dh - 1).astype(np.float32)
                pushed = np.empty_like(base)
                for ch in range(base.shape[2]):
                    pushed[:, :, ch] = cv2.remap(smoothed[:, :, ch], xx_push, yy_push, cv2.INTER_LINEAR)
                wt_mag = np.clip((mag - 5.0) / 30.0, 0.0, 0.85)[:, :, None]
                base = smoothed * (1.0 - wt_mag) + pushed * wt_mag
            Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), im.mode).save(out)
        return
    cmd = [str(exe), str(inp), str(out), '4']
    if mode: cmd += ['--mode', mode]
    cmd += extra
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    cands = [candidate(x) for x in sys.argv[1:]]
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        print(f'{"scene":11s} ' + '  '.join(f'{cname(c):>s}' for c in cands))
        for name in SCENES:
            truth = scene(name)
            low = down(truth)
            inp = td / (name + '.webp'); encode_pm(low).save(inp, lossless=True)
            cmask = checker_cells(low)
            row = []
            for cand in cands:
                out = td / (cname(cand).replace('/', '_').replace(' ', '_') + '-' + name + '.webp')
                run_candidate(cand, inp, out)
                got = pm(np.asarray(Image.open(out).convert('RGBA')))
                b = 4
                mae = float(np.abs(got[b:-b, b:-b] - truth[b:-b, b:-b]).mean())
                hg, hg95 = hourglass_amp(got, low)
                if cmask.any():
                    yy, xx = np.mgrid[0:N * S, 0:N * S]
                    cellmask = cmask[yy // S, xx // S]
                    chk = float(np.abs(got - truth)[cellmask].mean())
                else:
                    chk = float('nan')
                row.append((mae, hg, hg95, chk))
            print(f'{name:11s} ' + '  '.join(
                f'MAE {m:.5f} HG {h:.5f} HG95 {p:.5f} CHK {c:.5f}' for m, h, p, c in row))


if __name__ == '__main__':
    main()

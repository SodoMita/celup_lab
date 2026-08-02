#!/usr/bin/env python3
"""Before/after for the polar artifact: a clean circle upscaled 4x, zoomed at
the cardinal (top) point and a diagonal point, comparing autoblur (isotropic
ref) vs autodeblur -r6 (polar artifact: cardinal edge pulled in) vs -r1.5
(artifact gone, sub-pixel).  Output: images/sheet_polar.webp."""
import subprocess, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

LAB = Path(__file__).resolve().parent / "celup_lab"
OUT = Path(__file__).resolve().parent / "images"
OUT.mkdir(exist_ok=True)
S = 4


def lin(x):
    x = x / 255.
    return np.where(x <= .04045, x / 12.92, ((x + .055) / 1.055) ** 2.4)


def pm(a):
    a = np.asarray(a, dtype=np.float32)
    o = np.empty_like(a)
    o[..., :3] = lin(a[..., :3]) * (a[..., 3:4] / 255.)
    o[..., 3] = a[..., 3] / 255.
    return o


def encode_pm(p):
    a = np.clip(p[..., 3:4], 0, 1)
    rgb = np.clip(np.divide(p[..., :3], np.maximum(a, 1e-8)), 0, 1)
    sr = np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - .055)
    return Image.fromarray(np.dstack((np.clip(sr * 255, 0, 255),
                                      a[..., 0] * 255)).astype(np.uint8), 'RGBA')


def run(mode, extra, src):
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "o.webp"
        subprocess.run([str(LAB), str(src), str(out), "4", "--mode", mode,
                        "--max-mib", "2048", "-c", "linear", "-k", "bspline"] + extra,
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return np.asarray(Image.open(out).convert("RGB"), dtype=np.uint8)


# big clean circle
z = 96 * S
yy, xx = np.mgrid[0:z, 0:z]
cx = cy = z / 2.
R = z * 0.28
mask = (xx - cx) ** 2 + (yy - cy) ** 2 < R * R
a = np.zeros((z, z, 4), np.uint8)
a[...] = (236, 236, 240, 255)
a[mask] = (28, 36, 84, 255)
truth = pm(a)
low = truth.reshape(96, S, 96, S, 4).mean((1, 3))
with tempfile.TemporaryDirectory() as td:
    src = Path(td) / "c.webp"
    encode_pm(low).save(src, lossless=True)
    ab = run("autoblur", ["-r", "1.5"], src)
    r6 = run("autodeblur", ["-r", "6", "-s", "100", "-g", "64", "-D", "remap"], src)
    r15 = run("autodeblur", ["-r", "1.5", "-s", "100", "-D", "remap"], src)

# crop windows around cardinal (top: cx, cy-R) and diagonal (cx+R/√2, cy-R/√2)
half = 70
card_cy, card_cx = int(cy - R), int(cx)
diag_cy, diag_cx = int(cy - R * 0.707), int(cx + R * 0.707)


def crop(im, cyx, cxx):
    return im[cyx - half:cyx + half, cxx - half:cxx + half]


def zoom(im, cell):
    return Image.fromarray(im).resize((cell, cell), Image.NEAREST)


def fonts():
    try:
        ft = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 18)
        fc = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    except Exception:
        ft = fc = ImageFont.load_default()
    return ft, fc


COLS = ["autoblur (isotropic ref)", "autodeblur -r6 (POLAR artifact)",
        "autodeblur -r1.5 (clean)"]
imgs = {"card": [crop(ab, card_cy, card_cx), crop(r6, card_cy, card_cx), crop(r15, card_cy, card_cx)],
        "diag": [crop(ab, diag_cy, diag_cx), crop(r6, diag_cy, diag_cx), crop(r15, diag_cy, diag_cx)]}
cell = 300
labh = 40
rowh = 30
W = 3 * cell
H = labh + 2 * (rowh + cell) + 20
canvas = Image.new("RGB", (W, H), (8, 8, 10))
ft, fc = fonts()
ImageDraw.Draw(canvas).rectangle([0, 0, W, labh], fill=(12, 12, 16))
ImageDraw.Draw(canvas).text((8, 6), "POLAR ARTIFACT before/after: circle 4x, cardinal (top) vs diagonal point", (255, 255, 255), font=ft)
for i, c in enumerate(COLS):
    ImageDraw.Draw(canvas).text((i * cell + 8, labh - 18), c, (210, 230, 255), font=fc)
y = labh
for rname, rlabel in [("card", "CARDINAL point (top) -- watch the edge pulled inward / rect notch in -r6"),
                      ("diag", "DIAGONAL point (45deg) -- reference shape")]:
    ImageDraw.Draw(canvas).rectangle([0, y, W, y + rowh], fill=(24, 24, 30))
    ImageDraw.Draw(canvas).text((8, y + 7), rlabel, (255, 230, 180), font=fc)
    y += rowh
    for i, im in enumerate(imgs[rname]):
        canvas.paste(zoom(im, cell), (i * cell, y))
    y += cell + 10
canvas.save(OUT / "sheet_polar.webp", lossless=True)
print("wrote", OUT / "sheet_polar.webp")

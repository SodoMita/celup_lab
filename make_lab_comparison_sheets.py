#!/usr/bin/env python3
"""Generate example inputs and comparison sheets for celup_lab modes.

The examples are procedural high-resolution RGBA scenes.  Each scene is reduced
with a premultiplied-linear 4x4 box filter to make a 96x96 WebP input.  celup_lab
then upscales that input back to 384x384 with several modes.

Outputs:
  examples/                  low-res source WebPs and ground-truth PNGs
  comparison_sheets/          per-case sheets plus comparison_sheet.png
"""
import math
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parent
EXE = ROOT / "celup_lab"
EXAMPLE_DIR = ROOT / "examples"
SHEET_DIR = ROOT / "comparison_sheets"
S = 4
N = 96
Z = N * S

# Include every algorithm currently exposed by celup_lab.  Each tuple is
# (sheet label, --mode value, extra command-line options).
MODE_SPECS = (
    ("nearest", "nearest", ()),
    ("bilinear", "bilinear", ()),
    ("adaptive", "adaptive", ()),
    ("autodeblur", "autodeblur", ()),
    ("compress2x2 g1", "autodeblur", ("-D", "compress2x2", "-g", "1")),
    ("compress2x2 g5", "autodeblur", ("-D", "compress2x2", "-g", "5")),
    ("sdf", "sdf", ()),
)
MODE_LABELS = tuple(label for label, _, _ in MODE_SPECS)
# Classifier visualization is not a MAE candidate; appended to sheets as a
# separate labelled panel.
CLASSMAP_SPEC = ("classmap (R=edge G=chk B=junct)", "classmap", ())


def lin(x):
    x = x / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def srgb(x):
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def pm(a):
    a = np.asarray(a, dtype=np.float32)
    o = np.empty_like(a, dtype=np.float32)
    alpha = a[:, :, 3:4] / 255.0
    o[:, :, :3] = lin(a[:, :, :3]) * alpha
    o[:, :, 3:4] = alpha
    return o


def encode_pm(p):
    a = np.clip(p[:, :, 3:4], 0, 1)
    rgb = np.divide(p[:, :, :3], np.maximum(a, 1e-8))
    rgb = np.clip(rgb, 0, 1)
    rgba = np.dstack((np.clip(srgb(rgb) * 255, 0, 255), a[:, :, 0] * 255)).astype(np.uint8)
    return Image.fromarray(rgba, "RGBA")


def down4(a):
    """4x4 box reduction in premultiplied-linear RGBA."""
    return pm(a).reshape(N, S, N, S, 4).mean((1, 3))


def checker_composite(img):
    a = np.asarray(img.convert("RGBA"), dtype=np.uint16)
    h, w = a.shape[:2]
    yy, xx = np.indices((h, w))
    bg = np.where(((xx // 12 + yy // 12) & 1)[..., None], (210, 210, 210), (246, 246, 246)).astype(np.uint16)
    alpha = a[:, :, 3:4]
    rgb = (a[:, :, :3] * alpha + bg * (255 - alpha) + 127) // 255
    return Image.fromarray(rgb.astype(np.uint8), "RGB")


def grad_background(c0, c1):
    arr = np.zeros((Z, Z, 4), np.uint8)
    x = np.linspace(0, 1, Z, dtype=np.float32)[None, :]
    y = np.linspace(0, 1, Z, dtype=np.float32)[:, None]
    mix = np.clip(0.72 * x + 0.28 * y, 0, 1)
    for ch in range(3):
        arr[:, :, ch] = (c0[ch] * (1 - mix) + c1[ch] * mix).astype(np.uint8)
    arr[:, :, 3] = 255
    return Image.fromarray(arr, "RGBA")


def scene_badge():
    im = grad_background((22, 28, 54), (48, 94, 142))
    d = ImageDraw.Draw(im, "RGBA")
    d.rounded_rectangle((34*S, 14*S, 85*S, 75*S), radius=12*S, fill=(255, 204, 58, 255))
    d.rounded_rectangle((40*S, 20*S, 79*S, 69*S), radius=8*S, fill=(33, 45, 88, 255))
    d.polygon([(60*S, 26*S), (68*S, 50*S), (93*S, 50*S), (73*S, 64*S), (80*S, 88*S),
               (60*S, 74*S), (40*S, 88*S), (47*S, 64*S), (27*S, 50*S), (52*S, 50*S)],
              fill=(255, 100, 80, 255))
    d.line((17*S, 83*S, 88*S, 18*S), fill=(255, 255, 255, 255), width=2*S)
    return im


def scene_lines():
    im = Image.new("RGBA", (Z, Z), (235, 232, 216, 255))
    d = ImageDraw.Draw(im, "RGBA")
    for i in range(-24, 130, 14):
        d.line((i*S, 98*S, (i+78)*S, -6*S), fill=(54, 40, 104, 255), width=max(1, S))
    d.line((4*S, 48*S, 92*S, 54*S), fill=(225, 58, 52, 255), width=2*S)
    d.line((9*S, 80*S, 88*S, 18*S), fill=(20, 155, 190, 255), width=3*S)
    d.rectangle((20*S, 24*S, 75*S, 71*S), outline=(28, 28, 28, 255), width=S)
    return im


def scene_curves_alpha():
    im = Image.new("RGBA", (Z, Z), (0, 0, 0, 0))
    d = ImageDraw.Draw(im, "RGBA")
    d.ellipse((8*S, 9*S, 88*S, 88*S), fill=(20, 185, 255, 150))
    d.ellipse((22*S, 18*S, 74*S, 70*S), fill=(255, 210, 54, 220))
    d.ellipse((39*S, 31*S, 82*S, 84*S), fill=(216, 56, 132, 180))
    d.arc((10*S, 52*S, 94*S, 135*S), start=205, end=315, fill=(255, 255, 255, 255), width=2*S)
    d.line((12*S, 82*S, 84*S, 14*S), fill=(255, 78, 35, 255), width=2*S)
    return im


def scene_photoish():
    # Procedural photo-like texture: smooth sky/water gradients plus soft blobs.
    arr = np.zeros((Z, Z, 4), np.uint8)
    y = np.linspace(0, 1, Z, dtype=np.float32)[:, None]
    x = np.linspace(0, 1, Z, dtype=np.float32)[None, :]
    arr[:, :, 0] = np.clip(70 + 95 * x + 20 * y, 0, 255)
    arr[:, :, 1] = np.clip(100 + 90 * (1 - y) + 25 * np.sin(5 * x), 0, 255)
    arr[:, :, 2] = np.clip(140 + 70 * (1 - y), 0, 255)
    arr[:, :, 3] = 255
    im = Image.fromarray(arr, "RGBA")
    d = ImageDraw.Draw(im, "RGBA")
    d.ellipse((-10*S, 55*S, 45*S, 105*S), fill=(34, 116, 76, 230))
    d.ellipse((30*S, 47*S, 120*S, 112*S), fill=(42, 132, 72, 240))
    d.polygon([(0, 78*S), (28*S, 47*S), (55*S, 83*S), (84*S, 35*S), (Z, 82*S), (Z, Z), (0, Z)],
              fill=(72, 89, 98, 245))
    im = im.filter(ImageFilter.GaussianBlur(radius=0.35*S))
    d = ImageDraw.Draw(im, "RGBA")
    d.ellipse((68*S, 10*S, 84*S, 26*S), fill=(255, 225, 150, 255))
    return im


def scene_tiny_text():
    im = Image.new("RGBA", (Z, Z), (248, 248, 244, 255))
    d = ImageDraw.Draw(im, "RGBA")
    d.rounded_rectangle((8*S, 10*S, 88*S, 86*S), radius=5*S, fill=(30, 35, 52, 255))
    d.rectangle((14*S, 18*S, 82*S, 30*S), fill=(82, 195, 120, 255))
    for y in (39, 51, 63, 75):
        d.line((16*S, y*S, 80*S, (y-3)*S), fill=(235, 235, 220, 255), width=S)
    d.rectangle((18*S, 20*S, 28*S, 28*S), fill=(255, 230, 80, 255))
    d.rectangle((32*S, 20*S, 54*S, 28*S), fill=(255, 255, 255, 255))
    # Use a simple blocky vector word mark so it stays deterministic.
    for x0 in (18, 34, 50, 66):
        d.rectangle((x0*S, 34*S, (x0+6)*S, 35*S), fill=(255, 100, 80, 255))
        d.rectangle((x0*S, 34*S, (x0+1)*S, 44*S), fill=(255, 100, 80, 255))
    return im


def scene_crossing():
    # Antialiased crossing strokes/sections.  This stresses whether a method can
    # sharpen intersections without inventing new colours or fitted geometry.
    im = Image.new("RGBA", (Z, Z), (238, 235, 222, 255))
    d = ImageDraw.Draw(im, "RGBA")
    d.line((8*S, 82*S, 88*S, 18*S), fill=(35, 34, 90, 255), width=4*S)
    d.line((12*S, 18*S, 84*S, 84*S), fill=(210, 66, 52, 230), width=3*S)
    d.line((4*S, 50*S, 92*S, 50*S), fill=(30, 160, 180, 220), width=2*S)
    d.ellipse((38*S, 38*S, 58*S, 58*S), outline=(255, 245, 120, 255), width=S)
    return im


def scene_soft_gradient():
    # Broad smooth diagonal colour transition: useful for seeing whether an
    # algorithm creates small cell/triangle facets inside a true gradient.
    yy, xx = np.mgrid[0:Z, 0:Z].astype(np.float32)
    u = (xx * 0.78 + yy * 0.48) / Z
    t = np.clip((u - 0.28) / 0.62, 0, 1)
    t = t * t * (3 - 2 * t)
    c0 = np.array([24, 32, 68], np.float32)
    c1 = np.array([252, 188, 58], np.float32)
    arr = np.zeros((Z, Z, 4), np.uint8)
    arr[:, :, :3] = np.clip(c0 * (1 - t[..., None]) + c1 * t[..., None], 0, 255)
    arr[:, :, 3] = 255
    im = Image.fromarray(arr, "RGBA")
    d = ImageDraw.Draw(im, "RGBA")
    d.line((8*S, 86*S, 88*S, 12*S), fill=(255, 255, 255, 90), width=S)
    return im


def scene_checker1():
    # 1 source px checkerboard (Nyquist frequency): the hourglass/bow-tie
    # torture test.  Truth renders 4x4px blocks per source pixel.
    c = ((np.mgrid[0:Z, 0:Z][1] // S + np.mgrid[0:Z, 0:Z][0] // S) & 1)
    a = np.zeros((Z, Z, 4), np.uint8)
    a[:, :, 0] = np.where(c, 28, 238)
    a[:, :, 1] = np.where(c, 88, 218)
    a[:, :, 2] = np.where(c, 28, 238)
    a[:, :, 3] = 255
    return Image.fromarray(a, "RGBA")


def scene_checker2():
    # 2 source px checkerboard (resolvable, but locally checker-ambiguous):
    # does the algorithm keep intentional checker texture?
    c = ((np.mgrid[0:Z, 0:Z][1] // (2 * S) + np.mgrid[0:Z, 0:Z][0] // (2 * S)) & 1)
    a = np.zeros((Z, Z, 4), np.uint8)
    a[:, :, 0] = np.where(c, 40, 225)
    a[:, :, 1] = np.where(c, 190, 60)
    a[:, :, 2] = np.where(c, 60, 200)
    a[:, :, 3] = 255
    return Image.fromarray(a, "RGBA")


def scene_crosshatch():
    # Two families of 1-px-wide crossing diagonal lines: crossing artefacts,
    # ringing dots and invented crossing colours all show here.
    yy, xx = np.mgrid[0:Z, 0:Z]
    a = np.zeros((Z, Z, 4), np.uint8)
    a[:] = (250, 250, 252, 255)
    xh = (((xx - yy) % (8 * S)) < S) | (((xx + yy) % (8 * S)) < S)
    a[xh] = (25, 25, 130, 255)
    return Image.fromarray(a, "RGBA")


def scene_rings():
    # Thin concentric rings: curved near-Nyquist structures.
    yy, xx = np.mgrid[0:Z, 0:Z].astype(np.float32)
    r = np.sqrt((xx - Z / 2) ** 2 + (yy - Z / 2) ** 2)
    a = np.zeros((Z, Z, 4), np.uint8)
    a[:] = (24, 30, 44, 255)
    a[(r % (6 * S)) < (2 * S)] = (245, 200, 70, 255)
    return Image.fromarray(a, "RGBA")


SCENES = (
    ("badge", scene_badge),
    ("lines", scene_lines),
    ("curves_alpha", scene_curves_alpha),
    ("photoish", scene_photoish),
    ("crossing", scene_crossing),
    ("soft_gradient", scene_soft_gradient),
    ("tiny_text", scene_tiny_text),
    ("checker1", scene_checker1),
    ("checker2", scene_checker2),
    ("crosshatch", scene_crosshatch),
    ("rings", scene_rings),
)


def draw_label(draw, xy, text, fill="black"):
    draw.text(xy, text, fill=fill)


def safe_name(label):
    return label.replace(" ", "_").replace("/", "_")


def run_mode(inp, out, mode, extra=()):
    cmd = [str(EXE), str(inp), str(out), "4", "--mode", mode, *extra]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def make_sheet_for_scene(name, truth_img, low_img, outputs, metrics, classmap_img=None):
    labels_and_imgs = [("ground truth", truth_img), ("source x4", low_img.resize((Z, Z), Image.Resampling.NEAREST))]
    labels_and_imgs += [(m + f"\nMAE {metrics[m]:.5f}", outputs[m]) for m in MODE_LABELS]
    if classmap_img is not None:
        labels_and_imgs += [(CLASSMAP_SPEC[0], classmap_img)]
    cols = len(labels_and_imgs)
    label_h = 42
    sheet = Image.new("RGB", (cols * Z, Z + label_h), "white")
    d = ImageDraw.Draw(sheet)
    for i, (label, img) in enumerate(labels_and_imgs):
        x = i * Z
        draw_label(d, (x + 5, 4), label)
        sheet.paste(checker_composite(img), (x, label_h))
        d.rectangle((x, label_h, x + Z - 1, label_h + Z - 1), outline=(180, 180, 180))
    return sheet


def main():
    if not EXE.exists():
        raise SystemExit(f"Build {EXE} first")
    EXAMPLE_DIR.mkdir(exist_ok=True)
    SHEET_DIR.mkdir(exist_ok=True)
    panels = []
    summary_rows = []
    for name, maker in SCENES:
        truth = maker().convert("RGBA")
        low_pm = down4(np.asarray(truth))
        low = encode_pm(low_pm)
        truth_path = EXAMPLE_DIR / f"{name}_truth.png"
        low_webp = EXAMPLE_DIR / f"{name}_source_96.webp"
        low_png = EXAMPLE_DIR / f"{name}_source_96.png"
        truth.save(truth_path)
        low.save(low_png)
        low.save(low_webp, lossless=True)

        truth_pm = pm(np.asarray(truth))
        outputs = {}
        metrics = {}
        for label, mode, extra in MODE_SPECS:
            out_path = EXAMPLE_DIR / f"{name}_{safe_name(label)}_4x.webp"
            run_mode(low_webp, out_path, mode, extra)
            img = Image.open(out_path).convert("RGBA")
            outputs[label] = img
            got = pm(np.asarray(img))
            err = np.abs(got[4:-4, 4:-4] - truth_pm[4:-4, 4:-4])
            metrics[label] = float(err.mean())
        summary_rows.append((name, metrics))
        cm_path = EXAMPLE_DIR / f"{name}_classmap_4x.webp"
        run_mode(low_webp, cm_path, CLASSMAP_SPEC[1])
        classmap_img = Image.open(cm_path).convert("RGBA")
        sheet = make_sheet_for_scene(name, truth, low, outputs, metrics, classmap_img)
        sheet_path = SHEET_DIR / f"comparison_{name}.webp"
        sheet.save(sheet_path, lossless=True)
        panels.append((name, sheet))

    # Combined sheet with a title row for each scene.
    row_h = Z + 70
    combined = Image.new("RGB", (panels[0][1].width, row_h * len(panels)), "white")
    d = ImageDraw.Draw(combined)
    for i, (name, panel) in enumerate(panels):
        y = i * row_h
        d.text((6, y + 5), f"{name}: procedural source -> 96x96 premultiplied-linear box -> celup_lab 4x", fill="black")
        combined.paste(panel, (0, y + 26))
    combined.save(SHEET_DIR / "comparison_sheet.webp", lossless=True)

    # Compact source overview.
    overview = Image.new("RGB", (len(SCENES) * Z, Z + 42), "white")
    d = ImageDraw.Draw(overview)
    for i, (name, _) in enumerate(SCENES):
        low = Image.open(EXAMPLE_DIR / f"{name}_source_96.png").convert("RGBA")
        d.text((i * Z + 5, 4), name + " source x4", fill="black")
        overview.paste(checker_composite(low.resize((Z, Z), Image.Resampling.NEAREST)), (i * Z, 42))
    overview.save(SHEET_DIR / "example_sources.webp", lossless=True)

    # Markdown summary of MAE numbers.
    lines = ["# celup_lab generated comparison sheets", "", "Metric: mean absolute error in premultiplied-linear RGBA, excluding 4px border; lower is better.", ""]
    lines.append("| case | " + " | ".join(MODE_LABELS) + " |")
    lines.append("|---|" + "---|" * len(MODE_LABELS))
    for name, metrics in summary_rows:
        lines.append("| " + name + " | " + " | ".join(f"{metrics[m]:.5f}" for m in MODE_LABELS) + " |")
    lines += ["", "Generated files:", "", "- `comparison_sheets/comparison_sheet.webp`", "- `comparison_sheets/example_sources.webp`", "- `comparison_sheets/comparison_<case>.webp`", "- `examples/*_source_96.webp` and matching mode outputs"]
    (ROOT / "comparison_sheets.md").write_text("\n".join(lines) + "\n")

    print("Wrote", SHEET_DIR / "comparison_sheet.webp")
    print("Wrote", SHEET_DIR / "example_sources.webp")
    print("Wrote", ROOT / "comparison_sheets.md")


if __name__ == "__main__":
    main()

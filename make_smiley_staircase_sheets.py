#!/usr/bin/env python3
"""Generate comparison sheets for poor_smiley.webp and staircase evaluation.

Outputs:
  images/comparison_sheets/poor_smiley_comparison.png (.webp)
  images/comparison_sheets/poor_smiley_crop_comparison.png (.webp)
  images/comparison_sheets/staircase_comparison.png (.webp)
  images/comparison_sheets/staircase_diag45_comparison.png (.webp)
  images/tests/staircase_test.webp (.png)
"""
import os
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from tests.check_stairs import crossings

ROOT = Path(__file__).resolve().parent
EXE = ROOT / "celup_lab"
SMILEY_SRC = ROOT / "images/examples/poor_smiley.webp"
DIAG_SRC = ROOT / "tests/diagline48_src.webp"
CAT_SRC = ROOT / "images/examples/cat.webp"
PIKACHU_SRC = ROOT / "images/examples/pikachu.webp"
SHEET_DIR = ROOT / "images/comparison_sheets"
TEST_DIR = ROOT / "images/tests"

# List of modes to compare: (label, tool, mode, scale, extra_flags)
# tool can be 'celup_lab', 'pil', 'cv2', 'scipy', 'py', or 'source'
MODES = [
    ("Source (Nearest)", "source", "nearest", ()),
    ("Nearest Neighbor", "celup_lab", "nearest", ()),
    ("Bilinear", "celup_lab", "bilinear", ()),
    ("Triangle", "celup_lab", "triangle", ()),
    ("Scale2X", "celup_lab", "scale2x", ()),
    ("Cubic", "celup_lab", "cubic", ()),
    ("Mitchell", "celup_lab", "mitchell", ()),
    ("Lanczos2", "celup_lab", "lanczos2", ()),
    ("Lanczos3", "celup_lab", "lanczos3", ()),
    ("Dehourglass", "celup_lab", "dehourglass", ()),
    ("Blur", "celup_lab", "blur", ()),
    ("Compress", "celup_lab", "compress", ()),
    ("Safecompress s4", "celup_lab", "safecompress", ("--strength", "4")),
    ("Hourglassfix", "celup_lab", "consistentcompress", ("--strength", "4")),
    ("Hourglasscompress s4", "celup_lab", "hourglasscompress", ("--strength", "4")),
    ("Blurcompress s4", "celup_lab", "blurcompress", ("--strength", "4")),
    ("Safeblurcompress s4", "celup_lab", "safeblurcompress", ("--strength", "4")),
    ("Edgecompress s4", "celup_lab", "edgecompress", ("--strength", "4")),
    ("Deblurcompress s4", "celup_lab", "deblurcompress", ("--strength", "4", "--blur-radius", ".7")),
    ("Adaptive", "celup_lab", "adaptive", ()),
    ("Adaptive auto", "celup_lab", "adaptive", ("--checker-policy", "auto")),
    ("Adaptive s2x", "celup_lab", "adaptive", ("--checker-policy", "scale2x")),
    ("SDF", "celup_lab", "sdf", ()),
    ("Autoblur (auto)", "celup_lab", "autoblur", ()),
    ("Autodeblur (auto)", "celup_lab", "autodeblur", ()),
    ("autodeblur Smiley\n(-r 6 -s 100 -g 64 -D remap)", "celup_lab", "autodeblur",
     ("-c", "linear", "-k", "bspline", "-r", "6", "-s", "100", "-g", "64", "-D", "remap")),
    ("autodeblur Miya\n(-r 2.3 -s 100 -g 16 -D remap)", "celup_lab", "autodeblur",
     ("-c", "linear", "-k", "bspline", "-r", "2.3", "-s", "100", "-g", "16", "-D", "remap")),
    ("PIL Bicubic (Ref)", "pil", "bicubic", ()),
    ("PIL Lanczos (Ref)", "pil", "lanczos", ()),
    ("scipy:spline5 (Ref)", "scipy", "spline5", ()),
    ("cv2:lanczos4 (Ref)", "cv2", "lanczos4", ()),
    ("py:vector (Ref)", "py", "vector", ()),
]

def load_font(size):
    try:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    except Exception:
        return ImageFont.load_default()


def run_upscale(src_path, dst_path, scale, tool, mode, extra):
    if tool == "source":
        im = Image.open(src_path)
        w, h = im.size
        im.resize((w * scale, h * scale), Image.Resampling.NEAREST).save(dst_path)
    elif tool == "pil":
        im = Image.open(src_path)
        w, h = im.size
        res_map = {
            "nearest": Image.Resampling.NEAREST,
            "bilinear": Image.Resampling.BILINEAR,
            "bicubic": Image.Resampling.BICUBIC,
            "lanczos": Image.Resampling.LANCZOS,
        }
        res = res_map.get(mode, Image.Resampling.BICUBIC)
        im.resize((w * scale, h * scale), res).save(dst_path)
    elif tool == "cv2":
        import cv2
        im = Image.open(src_path)
        arr = np.asarray(im)
        w, h = im.size
        inter = {"nearest": cv2.INTER_NEAREST, "bilinear": cv2.INTER_LINEAR,
                 "cubic": cv2.INTER_CUBIC, "lanczos4": cv2.INTER_LANCZOS4}.get(mode, cv2.INTER_CUBIC)
        Image.fromarray(cv2.resize(arr, (w * scale, h * scale), interpolation=inter), im.mode).save(dst_path)
    elif tool == "scipy":
        from scipy.ndimage import zoom
        im = Image.open(src_path)
        arr = np.asarray(im, dtype=np.float32)
        order = 5 if mode == "spline5" else (3 if mode == "spline3" else 1)
        up = np.clip(zoom(arr, (scale, scale, 1), order=order), 0, 255).astype(np.uint8)
        Image.fromarray(up, im.mode).save(dst_path)
    elif tool == "py":
        import cv2
        im = Image.open(src_path)
        w, h = im.size
        dw, dh = w * scale, h * scale
        base = cv2.resize(np.asarray(im), (dw, dh), interpolation=cv2.INTER_LANCZOS4).astype(np.float32)
        if mode == "edgedir":
            lum = base[:, :, :3].mean(axis=2)
            gx = cv2.Sobel(lum, cv2.CV_32F, 1, 0, ksize=3); gy = cv2.Sobel(lum, cv2.CV_32F, 0, 1, ksize=3)
            mag = np.sqrt(gx**2 + gy**2) + 1e-6
            nx = gx / mag; ny = gy / mag
            yy, xx = np.mgrid[0:dh, 0:dw]
            xx_push = np.clip(xx - nx * 0.8, 0, dw - 1).astype(np.float32)
            yy_push = np.clip(yy - ny * 0.8, 0, dh - 1).astype(np.float32)
            pushed = cv2.remap(base, xx_push, yy_push, cv2.INTER_LINEAR)
            wt = np.clip((mag - 10.0) / 40.0, 0.0, 0.7)[:, :, None]
            base = base * (1.0 - wt) + pushed * wt
        elif mode == "vector":
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
                acc += wt * cv2.remap(base, xs, ys, cv2.INTER_LINEAR)
                w_tot += wt
            smoothed = acc / w_tot
            shift = 0.6
            xx_push = np.clip(xx - nx * shift, 0, dw - 1).astype(np.float32)
            yy_push = np.clip(yy - ny * shift, 0, dh - 1).astype(np.float32)
            pushed = cv2.remap(smoothed, xx_push, yy_push, cv2.INTER_LINEAR)
            wt_mag = np.clip((mag - 5.0) / 30.0, 0.0, 0.85)[:, :, None]
            base = smoothed * (1.0 - wt_mag) + pushed * wt_mag
        Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), im.mode).save(dst_path)
    else:
        cmd = [str(EXE), str(src_path), str(dst_path), str(scale), "--mode", mode, *extra]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def create_staircase_test_image(path_webp, path_png):
    from scipy.ndimage import gaussian_filter

    yy, xx = np.mgrid[0:48, 0:48]
    lum = np.ones((48, 48), dtype=np.float64)

    # Top left: Bold 45-degree diagonal half-plane edge
    lum[0:24, 0:24] = np.where((yy[0:24, 0:24] - xx[0:24, 0:24]) > 2, 0.08, 0.95)

    # Top right: Bold 30-degree shallow diagonal edge
    lum[0:24, 24:48] = np.where(yy[0:24, 24:48] > (0.5 * (xx[0:24, 24:48] - 24) + 6), 0.08, 0.95)

    # Bottom left: Bold circular disk contour
    dist_circ = np.sqrt((xx[24:48, 0:24] - 12) ** 2 + (yy[24:48, 0:24] - 36) ** 2)
    lum[24:48, 0:24] = np.where(dist_circ <= 8.5, 0.08, 0.95)

    # Bottom right: Bold square L-corner block
    lum[30:44, 30:44] = 0.08

    lum = gaussian_filter(lum, 0.4)
    lum = np.round(lum * 44.0) / 44.0
    rgba = np.dstack([lum, lum * 0.95 + 0.03, lum * 0.90 + 0.06, np.ones_like(lum)])
    rgba_u8 = (np.clip(rgba, 0, 1) * 255).astype(np.uint8)
    im = Image.fromarray(rgba_u8, "RGBA")
    im.save(path_webp, lossless=True)
    return im


def compute_diag45_metrics(img_path):
    try:
        xe = crossings(img_path)
        ys = np.where(~np.isnan(xe))[0]
        if len(ys) < 10:
            return None
        # exclude outer rounded tips
        ys = ys[(ys >= 20) & (ys <= 108)]
        if len(ys) < 5:
            return None
        xs = xe[ys]
        p = np.polyfit(ys, xs, 1)
        res = xs - np.polyval(p, ys)
        jumps = np.abs(np.diff(res))
        jump95 = np.percentile(jumps, 95)
        jumpmax = np.max(jumps)
        res95 = np.percentile(np.abs(res), 95)
        return {"jump95": jump95, "jumpmax": jumpmax, "res95": res95}
    except Exception:
        return None


def make_grid_sheet(title, panels, cols=5, label_h=52):
    font_label = load_font(12)
    font_title = load_font(14)
    rows = (len(panels) + cols - 1) // cols
    pw, ph = panels[0][1].size
    title_h = 36 if title else 0
    sheet_w = cols * pw
    sheet_h = title_h + rows * (ph + label_h)
    sheet = Image.new("RGB", (sheet_w, sheet_h), (248, 249, 250))
    d = ImageDraw.Draw(sheet)

    if title:
        d.text((12, 10), title, fill=(20, 25, 30), font=font_title)

    for idx, (label_text, img) in enumerate(panels):
        r = idx // cols
        c = idx % cols
        x = c * pw
        y = title_h + r * (ph + label_h)
        # label bar background
        d.rectangle((x, y, x + pw - 1, y + label_h - 1), fill=(235, 238, 242))
        d.rectangle((x, y + label_h - 1, x + pw - 1, y + label_h - 1), fill=(200, 205, 210))
        # draw text lines
        lines = label_text.strip().split("\n")
        for lidx, line in enumerate(lines):
            d.text((x + 6, y + 4 + lidx * 15), line, fill=(20, 25, 35), font=font_label)
        # paste image
        sheet.paste(img, (x, y + label_h))
        # border around cell
        d.rectangle(
            (x, y, x + pw - 1, y + label_h + ph - 1), outline=(180, 185, 190), width=1
        )

    return sheet


def main():
    SHEET_DIR.mkdir(parents=True, exist_ok=True)
    TEST_DIR.mkdir(parents=True, exist_ok=True)

    print("1. Creating staircase composite test image...")
    stair_webp = TEST_DIR / "staircase_test.webp"
    stair_png = TEST_DIR / "staircase_test.png"
    create_staircase_test_image(stair_webp, stair_png)

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)

        # --------------------------------------------------------------------
        # Sheet 1: poor_smiley_comparison (256x256 -> 512x512 2x upscale)
        # --------------------------------------------------------------------
        print("2. Generating poor_smiley_comparison sheets (2x upscale)...")
        smiley_panels = []
        smiley_crop_panels = []
        for label, tool, mode, extra in MODES:
            out_p = td / f"smiley_{tool}_{mode}_{len(smiley_panels)}.webp"
            run_upscale(SMILEY_SRC, out_p, 2, tool, mode, extra)
            im = Image.open(out_p).convert("RGB")
            smiley_panels.append((label, im))
            # central crop 256x256 around mouth/eyes (coordinates 128..384)
            crop_im = im.crop((128, 128, 384, 384))
            smiley_crop_panels.append((label, crop_im))

        sheet_smiley = make_grid_sheet(
            "poor_smiley.webp 2x Upscale Comparison (512x512)", smiley_panels, cols=8
        )
        sheet_smiley.save(SHEET_DIR / "poor_smiley_comparison.webp", lossless=True)
        print("   -> saved poor_smiley_comparison.png (.webp)")

        sheet_smiley_crop = make_grid_sheet(
            "poor_smiley.webp 2x Upscale Central Detail Crop (256x256)",
            smiley_crop_panels,
            cols=8,
        )
        sheet_smiley_crop.save(SHEET_DIR / "poor_smiley_crop_comparison.webp", lossless=True)
        print("   -> saved poor_smiley_crop_comparison.png (.webp)")

        # --------------------------------------------------------------------
        # Sheet 2: staircase_comparison (staircase_test.webp 96x96 -> 384x384 4x)
        # --------------------------------------------------------------------
        print("3. Generating staircase_comparison sheet (8x upscale)...")
        stair_panels = []
        for label, tool, mode, extra in MODES:
            out_p = td / f"stair_{tool}_{mode}_{len(stair_panels)}.webp"
            run_upscale(stair_webp, out_p, 8, tool, mode, extra)
            im = Image.open(out_p).convert("RGB")
            stair_panels.append((label, im))

        sheet_stair = make_grid_sheet(
            "Staircase Problem Comparison — 45° Line, 30° Line, Circle & L-Corner (8x Upscale of 48x48)",
            stair_panels,
            cols=8,
        )
        sheet_stair.save(SHEET_DIR / "staircase_comparison.webp", lossless=True)
        print("   -> saved staircase_comparison.webp")

        # --------------------------------------------------------------------
        # Sheet 3: staircase_diag45_comparison (diagline48_src.webp 64x64 -> 256x256 4x)
        #          with quantitative jump95 / jumpmax / res95 metrics
        # --------------------------------------------------------------------
        print("4. Generating staircase_diag45_comparison with quantitative metrics...")
        diag45_panels = []
        for label, tool, mode, extra in MODES:
            out_p = td / f"diag45_{tool}_{mode}_{len(diag45_panels)}.webp"
            run_upscale(DIAG_SRC, out_p, 4, tool, mode, extra)
            im = Image.open(out_p).convert("RGB")
            mets = compute_diag45_metrics(out_p)
            if mets:
                metric_str = (
                    f"jump95: {mets['jump95']:.3f}px | max: {mets['jumpmax']:.3f}px\n"
                    f"res95: {mets['res95']:.3f}px"
                )
            else:
                metric_str = "staircase / raw step"
            full_label = f"{label}\n{metric_str}"
            diag45_panels.append((full_label, im))

        sheet_diag45 = make_grid_sheet(
            "45° Diagonal Line Staircase Evaluation (tests/diagline48_src.webp 4x) — Quantitative Staircase Metrics",
            diag45_panels,
            cols=8,
            label_h=66,
        )
        sheet_diag45.save(SHEET_DIR / "staircase_diag45_comparison.webp", lossless=True)
        print("   -> saved staircase_diag45_comparison.webp")

        # --------------------------------------------------------------------
        # Sheet 5: cat_crop_comparison (cat.webp 128x128 crop -> 256x256 2x)
        # --------------------------------------------------------------------
        print("5. Generating cat_crop_comparison sheet (2x upscale detail crop)...")
        cat_src_crop = td / "cat_src_128.webp"
        Image.open(CAT_SRC).crop((136, 100, 264, 228)).save(cat_src_crop, lossless=True)
        cat_crop_panels = []
        for label, tool, mode, extra in MODES:
            out_p = td / f"cat_{tool}_{mode}_{len(cat_crop_panels)}.webp"
            run_upscale(cat_src_crop, out_p, 2, tool, mode, extra)
            im = Image.open(out_p).convert("RGB")
            cat_crop_panels.append((label, im))
        sheet_cat_crop = make_grid_sheet(
            "cat.webp 2x Upscale Detail Crop (256x256 around eyes/whiskers)",
            cat_crop_panels,
            cols=8,
        )
        sheet_cat_crop.save(SHEET_DIR / "cat_crop_comparison.webp", lossless=True)
        print("   -> saved cat_crop_comparison.webp")

        # --------------------------------------------------------------------
        # Sheet 6: pikachu_crop_comparison (pikachu.webp 128x128 crop -> 256x256 2x)
        # --------------------------------------------------------------------
        print("6. Generating pikachu_crop_comparison sheet (2x upscale line-art crop)...")
        pika_src_crop = td / "pika_src_128.webp"
        Image.open(PIKACHU_SRC).crop((100, 500, 228, 628)).save(pika_src_crop, lossless=True)
        pikachu_crop_panels = []
        for label, tool, mode, extra in MODES:
            out_p = td / f"pika_{tool}_{mode}_{len(pikachu_crop_panels)}.webp"
            run_upscale(pika_src_crop, out_p, 2, tool, mode, extra)
            im = Image.open(out_p).convert("RGB")
            pikachu_crop_panels.append((label, im))
        sheet_pika_crop = make_grid_sheet(
            "pikachu.webp 2x Upscale Line-Art Detail Crop (256x256 around vector contours)",
            pikachu_crop_panels,
            cols=8,
        )
        sheet_pika_crop.save(SHEET_DIR / "pikachu_crop_comparison.webp", lossless=True)
        print("   -> saved pikachu_crop_comparison.webp")

    print("Done! All comparison sheets created successfully.")


if __name__ == "__main__":
    main()

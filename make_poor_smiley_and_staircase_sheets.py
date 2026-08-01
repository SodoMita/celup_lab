#!/usr/bin/env python3
"""Comparison sheets for poor smiley and staircase problem (v5.0)."""
import subprocess, os
from pathlib import Path
from PIL import Image, ImageDraw
import numpy as np

ROOT = Path(__file__).resolve().parent
EXE = ROOT / "celup_lab"
SHEET_DIR = ROOT / "comparison_sheets"
SHEET_DIR.mkdir(exist_ok=True)

# ensure env for libwebp
ENV = os.environ.copy()
ENV["LD_LIBRARY_PATH"] = "/usr/lib/x86_64-linux-gnu:" + ENV.get("LD_LIBRARY_PATH","")

def run_celup(inp, out, scale, mode, extra=()):
    cmd = [str(EXE), str(inp), str(out), str(scale), "--mode", mode, *extra, "--max-mib", "2048"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)

def pil_resize(inp_path, out_path, scale, resample):
    im = Image.open(inp_path).convert("RGBA")
    w,h = im.size
    out = im.resize((int(w*scale), int(h*scale)), resample=resample)
    out.save(out_path, lossless=True)

def checker_composite(img, cell=12):
    a = np.asarray(img.convert("RGBA"), dtype=np.uint16)
    h,w = a.shape[:2]
    yy,xx = np.indices((h,w))
    bg = np.where(((xx//cell + yy//cell)&1)[...,None], (210,210,210), (246,246,246)).astype(np.uint16)
    alpha = a[:,:,3:4]
    rgb = (a[:,:,:3]*alpha + bg*(255-alpha)+127)//255
    return Image.fromarray(rgb.astype(np.uint8), "RGB")

def make_sheet(images_labels, sheet_path, thumb_size=None, cols=4, label_h=36):
    # images_labels: list of (label, PIL Image)
    if not images_labels:
        return
    # determine thumb size: use first image size or thumb_size
    if thumb_size is None:
        tw,th = images_labels[0][1].size
    else:
        tw,th = thumb_size
    rows = (len(images_labels)+cols-1)//cols
    W = cols*tw
    H = rows*(th+label_h)
    sheet = Image.new("RGB", (W,H), "white")
    draw = ImageDraw.Draw(sheet)
    for i,(label, img) in enumerate(images_labels):
        r = i//cols
        c = i%cols
        x = c*tw
        y = r*(th+label_h)
        # label
        draw.text((x+4, y+2), label, fill="black")
        # paste
        # resize img to thumb if needed (for consistent grid)
        if img.size != (tw,th):
            # center crop or resize? For this sheet we want exact size, so resize if diff (should already be)
            # Use nearest for label composite? Just resize with high quality
            img2 = img.resize((tw,th), Image.LANCZOS)
        else:
            img2 = img
        sheet.paste(checker_composite(img2), (x, y+label_h))
        draw.rectangle((x, y+label_h, x+tw-1, y+label_h+th-1), outline=(180,180,180))
    sheet.save(sheet_path)
    print(f"Wrote {sheet_path} ({W}x{H})")

# ---- poor smiley sheet ----
poor = ROOT / "images/poor smiley.webp"
if poor.exists():
    print("Generating poor smiley sheet 2x")
    scale = 2
    tmpdir = Path("/tmp/smiley_sheet")
    tmpdir.mkdir(exist_ok=True)
    imgs = []
    # PIL baselines
    for name, res in [("PIL nearest", Image.NEAREST), ("PIL bilinear", Image.BILINEAR), ("PIL bicubic", Image.BICUBIC), ("PIL lanczos", Image.LANCZOS)]:
        out = tmpdir / f"pil_{name.split()[-1]}.webp"
        pil_resize(poor, out, scale, res)
        imgs.append((name, Image.open(out)))
    # celup modes
    mode_list = [
        ("nearest", "nearest", []),
        ("bilinear", "bilinear", []),
        ("triangle", "triangle", []),
        ("smooth", "smooth", []),
        ("smooth r1.5", "smooth", ["-r","1.5"]),
        ("cubic", "cubic", []),
        ("mitchell", "mitchell", []),
        ("lanczos3", "lanczos3", []),
        ("adaptive lowpass", "adaptive", []),
        ("adaptive auto", "adaptive", ["--checker-policy","auto"]),
        ("adaptive s2x", "adaptive", ["--checker-policy","scale2x"]),
        ("autoblur auto", "autoblur", []),
        ("autoblur bspline r0.5", "autoblur", ["-k","bspline","-c","linear","-r","0.5"]),
        ("autoblur bspline r1.5", "autoblur", ["-k","bspline","-c","linear","-r","1.5"]),
        ("autodeblur auto", "autodeblur", []),
        ("autodeblur r2.3 g16 (miya)", "autodeblur", ["-k","bspline","-c","linear","-r","2.3","-g","16","-D","remap"]),
        ("autodeblur r6 g64 (smiley user)", "autodeblur", ["-k","bspline","-c","linear","-r","6","-g","64","-D","remap","-s","100"]),
        ("autodeblur r6 g16", "autodeblur", ["-k","bspline","-c","linear","-r","6","-g","16","-D","remap"]),
        ("autodeblur r1.5 g8", "autodeblur", ["-k","bspline","-c","linear","-r","1.5","-g","8","-D","remap"]),
        ("sdf", "sdf", []),
    ]
    for label, mode, extra in mode_list:
        out = tmpdir / f"celup_{label.replace(' ','_').replace('(','').replace(')','')}.webp"
        try:
            run_celup(poor, out, scale, mode, extra)
            imgs.append((label, Image.open(out)))
        except subprocess.CalledProcessError as e:
            print(f"FAIL {label}: {e}")
    # make sheet, thumb size = poor*scale
    poor_im = Image.open(poor)
    tw = int(poor_im.width*scale)
    th = int(poor_im.height*scale)
    make_sheet(imgs, SHEET_DIR / "poor_smiley_2x_sheet.png", thumb_size=(tw,th), cols=4, label_h=38)

    # also 4x sheet for poor smiley with fewer modes (to keep size reasonable)
    print("Generating poor smiley sheet 4x subset")
    scale=4
    imgs4=[]
    # reuse some
    for label, mode, extra in [
        ("bilinear", "bilinear", []),
        ("triangle", "triangle", []),
        ("mitchell", "mitchell", []),
        ("lanczos3", "lanczos3", []),
        ("adaptive", "adaptive", []),
        ("autoblur", "autoblur", []),
        ("autodeblur r2.3 g16", "autodeblur", ["-k","bspline","-c","linear","-r","2.3","-g","16","-D","remap"]),
        ("autodeblur r6 g64", "autodeblur", ["-k","bspline","-c","linear","-r","6","-g","64","-D","remap","-s","100"]),
    ]:
        out = tmpdir / f"4x_{label.replace(' ','_')}.webp"
        run_celup(poor, out, scale, mode, extra)
        imgs4.append((label, Image.open(out)))
    tw = int(poor_im.width*scale)
    th = int(poor_im.height*scale)
    make_sheet(imgs4, SHEET_DIR / "poor_smiley_4x_subset.png", thumb_size=(tw,th), cols=4, label_h=30)

# ---- staircase sheet ----
diag = ROOT / "tests/diagline48_src.webp"
if diag.exists():
    print("Generating staircase sheet")
    scale=4
    tmpdir = Path("/tmp/stair_sheet")
    tmpdir.mkdir(exist_ok=True)
    imgs=[]
    # PIL
    for name, res in [("PIL nearest\nstaircase", Image.NEAREST), ("PIL bilinear\nsmooth", Image.BILINEAR), ("PIL bicubic\n0.489 jump", Image.BICUBIC), ("PIL lanczos\n0.269", Image.LANCZOS)]:
        out = tmpdir / f"pil_{name.split()[1]}.webp"
        pil_resize(diag, out, scale, res)
        imgs.append((name, Image.open(out)))
    # celup modes with metrics from earlier run (approx)
    mode_list = [
        ("celup nearest\n3.0 tread", "nearest", []),
        ("bilinear\n0.38", "bilinear", []),
        ("cubic\n0.508", "cubic", []),
        ("mitchell\n0.267", "mitchell", []),
        ("lanczos3\n0.229", "lanczos3", []),
        ("triangle\n0.014", "triangle", []),
        ("smooth\n0.023 v5.2", "smooth", []),
        ("autoblur bspline r0.5\n0.024 v5 fix (was 0.73)", "autoblur", ["-k","bspline","-c","linear","-r","0.5"]),
        ("autoblur auto\n0.382", "autoblur", []),
        ("adaptive\n0.365 v5.1 AA", "adaptive", []),
        ("autodeblur r2.3 g16\n0.017 smoothest", "autodeblur", ["-k","bspline","-c","linear","-r","2.3","-g","16","-D","remap"]),
        ("autodeblur r6 g64\n0.057", "autodeblur", ["-k","bspline","-c","linear","-r","6","-g","64","-D","remap","-s","100"]),
        ("autodeblur r0.5 g64\nold probe now smooth", "autodeblur", ["-k","bspline","-c","linear","-r","0.5","-g","64","-D","remap"]),
        ("sdf", "sdf", []),
    ]
    for label, mode, extra in mode_list:
        out = tmpdir / f"celup_{label.split()[0]}_{label.split()[1] if len(label.split())>1 else ''}.webp".replace("/","").replace("\n","_")
        try:
            run_celup(diag, out, scale, mode, extra)
            imgs.append((label, Image.open(out)))
        except Exception as e:
            print(f"FAIL {label}: {e}")
    # thumb 64*4=256
    make_sheet(imgs, SHEET_DIR / "staircase_45deg_sheet.png", thumb_size=(256,256), cols=4, label_h=48)

    # extra sheet showing param effect
    print("Generating param effect sheet on diagline")
    param_imgs=[]
    for g in ["1","4","16","64"]:
        out = tmpdir / f"param_g{g}.webp"
        run_celup(diag, out, 4, "autodeblur", ["-k","bspline","-c","linear","-r","2.3","-g",g,"-D","remap"])
        param_imgs.append((f"r2.3 g={g}", Image.open(out)))
    for s in ["1","4","16","100"]:
        out = tmpdir / f"param_s{s}.webp"
        run_celup(diag, out, 4, "autodeblur", ["-k","bspline","-c","linear","-r","2.3","-s",s,"-D","remap"])
        param_imgs.append((f"r2.3 s={s}", Image.open(out)))
    for r in ["0.5","1.5","2.3","6"]:
        out = tmpdir / f"param_r{r}.webp"
        run_celup(diag, out, 4, "autodeblur", ["-k","bspline","-c","linear","-r",r,"-g","16","-D","remap"])
        param_imgs.append((f"r={r} g16", Image.open(out)))
    make_sheet(param_imgs, SHEET_DIR / "staircase_param_effect.png", thumb_size=(256,256), cols=4, label_h=30)

print("Done sheets in", SHEET_DIR)

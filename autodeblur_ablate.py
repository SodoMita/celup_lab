#!/usr/bin/env python3
"""
Autodeblur A/B Feature Ablation Tool
=====================================
Systematically disables autodeblur features and measures the effect on
reconstruction quality and artifact metrics.

Usage:
  python3 autodeblur_ablate.py [image.webp ...] [--scale 2] [--output-dir results/]

Each "feature set" is a combination of env vars passed to celup_lab.
The tool runs celup_lab with each feature set, then computes:
  - MSE vs source (2x downsample then upsample)
  - PSNR, SSIM
  - Artifact metrics: speckle score, ringing score, staircase score

Feature sets (each disables one group from the baseline):
  baseline          — all features enabled (current merge1 defaults)
  no_amp            — CELUP_NOAMP=1    (disable narrow-feature amplitude restoration)
  no_terrace        — CELUP_NOTER=1    (disable terrace cleanup)
  no_dip            — CELUP_NODIP=1    (disable dip/line-class claim)
  no_peel           — CELUP_NOPEEL=1   (disable contour peeling)
  no_z              — CELUP_NOZ=1      (disable erf-gain map)
  no_trust          — CDG=0,1          (disable trust gates, let everything through)
  tight_trust       — CDG=0.01,0.05    (tighter trust gates)
  no_hull           — CELUP_NOHULL=1   (disable hull clamp)
  no_speckle        — CELUP_NOSPECKLE=1 (disable adaptive speckle suppression)
  method_push       — -D push          (force method 2 instead of auto)
  method_remap      — -D remap         (force method 1 instead of auto)
  method_analytical — -D analytical    (method 3, PCA gradient push)
"""

import argparse
import os
import subprocess
import sys
import json
import numpy as np
from pathlib import Path
from PIL import Image

# ---------------------------------------------------------------------------
# Feature sets — each is a dict of env vars (+ optional extra CLI args)
# ---------------------------------------------------------------------------
FEATURE_SETS = {
    "baseline": {
        "env": {},
        "extra_args": [],
        "desc": "All features enabled (current merge1 defaults)",
    },
    "no_amp": {
        "env": {"CELUP_NOAMP": "1"},
        "extra_args": [],
        "desc": "Disable narrow-feature amplitude restoration",
    },
    "no_terrace": {
        "env": {"CELUP_NOTER": "1"},
        "extra_args": [],
        "desc": "Disable terrace cleanup",
    },
    "no_dip": {
        "env": {"CELUP_NODIP": "1"},
        "extra_args": [],
        "desc": "Disable dip/line-class claim",
    },
    "no_peel": {
        "env": {"CELUP_NOPEEL": "1"},
        "extra_args": [],
        "desc": "Disable contour peeling",
    },
    "no_z": {
        "env": {"CELUP_NOZ": "1"},
        "extra_args": [],
        "desc": "Disable erf-gain map (ZM)",
    },
    "no_hull": {
        "env": {"CELUP_NOHULL": "1"},
        "extra_args": [],
        "desc": "Disable hull clamp (allow output to leave observed colour range)",
    },
    "no_speckle": {
        "env": {"CELUP_NOSPECKLE": "1"},
        "extra_args": [],
        "desc": "Disable adaptive speckle suppression in base render",
    },
    "no_trust": {
        "env": {"CDG": "0,1"},
        "extra_args": [],
        "desc": "Disable trust gates (CDG=0,1 lets everything through)",
    },
    "tight_trust": {
        "env": {"CDG": "0.01,0.05"},
        "extra_args": [],
        "desc": "Tighter trust gates (CDG=0.01,0.05)",
    },
    "method_push": {
        "env": {},
        "extra_args": ["-D", "push"],
        "desc": "Force method 2 (push) instead of auto",
    },
    "method_remap": {
        "env": {},
        "extra_args": ["-D", "remap"],
        "desc": "Force method 1 (remap) instead of auto",
    },
    "method_analytical": {
        "env": {},
        "extra_args": ["-D", "analytical"],
        "desc": "Method 3 (PCA gradient push / analog)",
    },
}

# ---------------------------------------------------------------------------
# Artifact metrics
# ---------------------------------------------------------------------------

def compute_speckle_score(img_arr):
    """
    Speckle detection: isolated pixels that differ significantly from their
    3x3 neighborhood median.  Returns (fraction_of_speckle_pixels, mean_speckle_magnitude).

    A high fraction means lots of isolated noise/artifacts.
    """
    from scipy.ndimage import median_filter
    gray = np.mean(img_arr[:, :, :3].astype(np.float32), axis=2)
    med = median_filter(gray, size=3)
    diff = np.abs(gray - med)
    # A pixel is "speckle" if it differs from the 3x3 median by more than 8
    speckle_mask = diff > 8.0
    fraction = speckle_mask.mean()
    mean_mag = diff[speckle_mask].mean() if speckle_mask.any() else 0.0
    return fraction, mean_mag


def compute_ringing_score(img_arr):
    """
    Ringing / halo detection: look for oscillations perpendicular to edges.
    Measure: Laplacian variance in a band around strong edges.

    High score = more ringing/overshoot.
    """
    from scipy.ndimage import laplace, sobel
    gray = np.mean(img_arr[:, :, :3].astype(np.float64), axis=2)
    # Edge map
    sx = sobel(gray, axis=1)
    sy = sobel(gray, axis=0)
    edge_mag = np.sqrt(sx**2 + sy**2)
    # Pixels near strong edges
    edge_mask = edge_mag > np.percentile(edge_mag, 95)
    # Dilate the mask by a few pixels (ringing band)
    from scipy.ndimage import binary_dilation
    ring_band = binary_dilation(edge_mask, iterations=3) & ~edge_mask
    # Laplacian in the ringing band
    lap = np.abs(laplace(gray))
    ring_score = lap[ring_band].mean() if ring_band.any() else 0.0
    return ring_score


def compute_staircase_score(img_arr):
    """
    Staircase detection: look for step-like patterns along diagonal edges.
    Measure: count transitions where the gradient direction is inconsistent
    with its neighbors (quantized diagonal instead of smooth).

    High score = more staircase artifacts.
    """
    gray = np.mean(img_arr[:, :, :3].astype(np.float64), axis=2)
    h, w = gray.shape
    if h < 4 or w < 4:
        return 0.0

    # Compute gradient direction at each pixel
    gx = np.diff(gray, axis=1)[:, :-1]  # (h, w-2)
    gy = np.diff(gray, axis=0)[:-1, :]  # (h-2, w)

    # We need them at the same size
    min_h = min(gx.shape[0], gy.shape[0]) - 1
    min_w = min(gx.shape[1], gy.shape[1]) - 1
    if min_h < 2 or min_w < 2:
        return 0.0

    gx = gx[:min_h, :min_w]
    gy = gy[:min_h, :min_w]

    mag = np.sqrt(gx**2 + gy**2) + 1e-8
    # Only consider pixels with significant gradient
    edge_mask = mag > np.percentile(mag, 75)

    # Angle of gradient
    angle = np.arctan2(gy, gx)

    # Check consistency of angle with neighbors
    angle_diff = np.zeros_like(angle)
    count = np.zeros_like(angle)
    for dy, dx in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
        sy = slice(max(0, dy), min_h + min(0, dy))
        sx = slice(max(0, dx), min_w + min(0, dx))
        ty = slice(max(0, -dy), min_h + min(0, -dy))
        tx = slice(max(0, -dx), min_w + min(0, -dx))
        a1 = angle[sy, sx]
        a2 = angle[ty, tx]
        diff = np.abs(a1 - a2)
        diff = np.minimum(diff, 2 * np.pi - diff)  # wrap
        angle_diff[sy, sx] += diff
        count[sy, sx] += 1

    angle_diff = angle_diff / np.maximum(count, 1)
    # Staircase pixels: high angle inconsistency on edges
    staircase_mask = (angle_diff > 0.5) & edge_mask
    staircase_score = staircase_mask.mean()
    return staircase_score


def compute_halo_score(img_arr):
    """
    Halo detection: bright/dark overshoot bands adjacent to edges.
    Look for pixels just outside strong edges that are brighter than
    both the edge and the background.

    High score = more halo artifacts.
    """
    gray = np.mean(img_arr[:, :, :3].astype(np.float64), axis=2)
    from scipy.ndimage import sobel, binary_dilation, uniform_filter
    sx = sobel(gray, axis=1)
    sy = sobel(gray, axis=0)
    edge_mag = np.sqrt(sx**2 + sy**2)

    # Strong edge mask
    edge_mask = edge_mag > np.percentile(edge_mag, 90)
    # Band just outside edges
    outer_band = binary_dilation(edge_mask, iterations=2) & ~edge_mask

    # Local mean for context
    local_mean = uniform_filter(gray, size=5)

    # Halo: pixels in outer band that deviate significantly from local mean
    deviation = np.abs(gray - local_mean)
    halo_score = deviation[outer_band].mean() if outer_band.any() else 0.0
    return halo_score


def compute_all_metrics(img_arr, source_arr=None):
    """Compute all metrics for an upscaled image."""
    metrics = {}

    # Artifact metrics (don't need source)
    sp_frac, sp_mag = compute_speckle_score(img_arr)
    metrics["speckle_fraction"] = sp_frac
    metrics["speckle_magnitude"] = sp_mag

    metrics["ringing_score"] = compute_ringing_score(img_arr)
    metrics["staircase_score"] = compute_staircase_score(img_arr)
    metrics["halo_score"] = compute_halo_score(img_arr)

    # Quality metrics (need source — compare 2x downsample of output vs source)
    if source_arr is not None:
        oh, ow = img_arr.shape[:2]
        sh, sw = source_arr.shape[:2]
        scale = oh // sh
        if scale >= 1 and oh == sh * scale and ow == sw * scale:
            down = img_arr[::scale, ::scale]
            if down.shape[:2] == source_arr.shape[:2]:
                diff = down.astype(float) - source_arr.astype(float)
                mse = np.mean(diff**2)
                metrics["mse_vs_source"] = mse
                if mse > 0:
                    metrics["psnr_vs_source"] = 10.0 * np.log10(255.0**2 / mse)
                else:
                    metrics["psnr_vs_source"] = float("inf")

                # SSIM
                from skimage.metrics import structural_similarity as ssim
                metrics["ssim_vs_source"] = ssim(
                    source_arr[:, :, :3], down[:, :, :3],
                    channel_axis=2, data_range=255
                )

                # Pixel-identical fraction
                metrics["pct_identical"] = (np.abs(diff) == 0).mean() * 100
                metrics["max_diff"] = int(np.abs(diff).max())
                metrics["mean_diff"] = float(np.abs(diff).mean())

    return metrics


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Autodeblur feature ablation tool")
    parser.add_argument("images", nargs="+", help="Input images (webp/png)")
    parser.add_argument("--scale", type=int, default=2, help="Upscale factor (default: 2)")
    parser.add_argument("--output-dir", default="ablate_results", help="Output directory")
    parser.add_argument("--exe", default=None, help="Path to celup_lab binary")
    parser.add_argument("--features", nargs="*", default=None,
                        help="Feature sets to test (default: all)")
    parser.add_argument("--save-images", action="store_true",
                        help="Save output images (for visual inspection)")
    args = parser.parse_args()

    # Find binary
    exe = args.exe
    if not exe:
        # Try workspace first
        candidates = [
            "/home/user/celup_lab/celup_lab",
            "./celup_lab",
        ]
        for c in candidates:
            if os.path.isfile(c) and os.access(c, os.X_OK):
                exe = c
                break
    if not exe:
        print("Error: cannot find celup_lab binary", file=sys.stderr)
        sys.exit(1)
    print(f"Using binary: {exe}")

    # Select feature sets
    feature_names = args.features or list(FEATURE_SETS.keys())
    for name in feature_names:
        if name not in FEATURE_SETS:
            print(f"Warning: unknown feature set '{name}', skipping")
    feature_names = [n for n in feature_names if n in FEATURE_SETS]

    os.makedirs(args.output_dir, exist_ok=True)

    all_results = {}

    for img_path in args.images:
        img_name = Path(img_path).stem
        print(f"\n=== Image: {img_name} ===")

        # Load source
        source = np.array(Image.open(img_path).convert("RGBA"))
        print(f"  Source shape: {source.shape}")

        img_results = {}

        for fname in feature_names:
            fset = FEATURE_SETS[fname]
            print(f"\n  --- Feature set: {fname} ---")
            print(f"  {fset['desc']}")
            if fset["env"]:
                print(f"  Env: {fset['env']}")
            if fset["extra_args"]:
                print(f"  Extra args: {fset['extra_args']}")

            # Output path
            out_path = os.path.join(args.output_dir, f"{img_name}_{fname}.webp")
            if not args.save_images:
                out_path = os.path.join(args.output_dir, f"tmp_{img_name}_{fname}.webp")

            # Build command
            cmd = [exe, img_path, out_path, str(args.scale),
                   "--mode", "autodeblur"] + fset["extra_args"]

            # Run with env
            env = os.environ.copy()
            env.update(fset["env"])

            try:
                result = subprocess.run(
                    cmd, env=env, capture_output=True, text=True, timeout=120
                )
                if result.returncode != 0:
                    print(f"  ERROR: {result.stderr.strip()}")
                    img_results[fname] = {"error": result.stderr.strip()}
                    continue
                if result.stderr:
                    # Extract method info from stderr
                    for line in result.stderr.strip().split("\n"):
                        if "method" in line.lower() or "kernel" in line.lower() or "sigma" in line.lower():
                            print(f"  {line}")
            except subprocess.TimeoutExpired:
                print(f"  TIMEOUT")
                img_results[fname] = {"error": "timeout"}
                continue

            # Load output
            if not os.path.isfile(out_path):
                print(f"  ERROR: output file not created")
                img_results[fname] = {"error": "no output file"}
                continue

            output = np.array(Image.open(out_path).convert("RGBA"))

            # Compute metrics
            metrics = compute_all_metrics(output, source)
            img_results[fname] = metrics

            # Print summary
            print(f"  MSE: {metrics.get('mse_vs_source', 'N/A'):.1f}" if isinstance(metrics.get('mse_vs_source'), float) else f"  MSE: N/A")
            print(f"  PSNR: {metrics.get('psnr_vs_source', 'N/A'):.2f}" if isinstance(metrics.get('psnr_vs_source'), float) else f"  PSNR: N/A")
            print(f"  SSIM: {metrics.get('ssim_vs_source', 'N/A'):.4f}" if isinstance(metrics.get('ssim_vs_source'), float) else f"  SSIM: N/A")
            print(f"  Speckle: {metrics['speckle_fraction']:.4f} (frac), {metrics['speckle_magnitude']:.1f} (mag)")
            print(f"  Ringing: {metrics['ringing_score']:.3f}")
            print(f"  Staircase: {metrics['staircase_score']:.4f}")
            print(f"  Halo: {metrics['halo_score']:.3f}")

            # Clean up temp file
            if not args.save_images and os.path.exists(out_path):
                os.remove(out_path)

        all_results[img_name] = img_results

    # Save results as JSON
    results_path = os.path.join(args.output_dir, "ablate_results.json")
    with open(results_path, "w") as f:
        # Convert numpy types to Python native for JSON
        def convert(obj):
            if isinstance(obj, (np.integer,)):
                return int(obj)
            if isinstance(obj, (np.floating,)):
                return float(obj)
            if isinstance(obj, np.ndarray):
                return obj.tolist()
            raise TypeError(f"Object of type {type(obj)} is not JSON serializable")
        json.dump(all_results, f, indent=2, default=convert)
    print(f"\nResults saved to {results_path}")

    # Print comparison table
    print("\n" + "=" * 100)
    print("COMPARISON TABLE")
    print("=" * 100)

    for img_name, img_results in all_results.items():
        print(f"\n--- {img_name} ---")
        # Header
        print(f"{'Feature Set':<22} {'MSE':>8} {'PSNR':>8} {'SSIM':>8} {'Speck%':>8} {'Ring':>8} {'Stair':>8} {'Halo':>8}")
        print("-" * 90)
        for fname, metrics in img_results.items():
            if "error" in metrics:
                print(f"{fname:<22} ERROR: {metrics['error']}")
                continue
            mse = metrics.get('mse_vs_source', float('nan'))
            psnr = metrics.get('psnr_vs_source', float('nan'))
            ssim = metrics.get('ssim_vs_source', float('nan'))
            sp = metrics.get('speckle_fraction', 0) * 100
            ring = metrics.get('ringing_score', 0)
            stair = metrics.get('staircase_score', 0)
            halo = metrics.get('halo_score', 0)
            print(f"{fname:<22} {mse:>8.1f} {psnr:>8.2f} {ssim:>8.4f} {sp:>7.3f}% {ring:>8.3f} {stair:>8.4f} {halo:>8.3f}")


if __name__ == "__main__":
    main()

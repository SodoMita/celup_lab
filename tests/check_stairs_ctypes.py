#!/usr/bin/env python3
"""v4.9.1 staircase gate using Python stdlib + libwebp."""
import ctypes, subprocess, sys, tempfile, os, math

webp = ctypes.CDLL('/usr/lib/x86_64-linux-gnu/libwebp.so.7')
webp.WebPDecodeRGBA.restype = ctypes.POINTER(ctypes.c_uint8)

def decode(path):
    data = open(path, 'rb').read()
    w, h = ctypes.c_int(), ctypes.c_int()
    ptr = webp.WebPDecodeRGBA(data, len(data), ctypes.byref(w), ctypes.byref(h))
    buf = bytes(ptr[:w.value * h.value * 4])
    webp.WebPFree(ptr)
    return w.value, h.value, buf

LAB = "./celup_lab"
SRC = "tests/diagline48_src.webp"

SHIP2 = ["--mode", "autodeblur", "--max-mib", "2048", "-c", "linear",
         "-k", "bspline", "-r", "6", "-s", "100", "-g", "64", "-D", "remap"]
SHIP4 = ["--mode", "autodeblur", "--max-mib", "2048", "-c", "linear",
         "-k", "bspline", "-r", "2.3", "-s", "100", "-g", "16", "-D", "remap"]
PROBE = ["--mode", "autodeblur", "--max-mib", "2048", "-c", "linear",
         "-k", "bspline", "-r", "0.5", "-s", "100", "-g", "64", "-D", "remap"]

TREADRUN_MAX = 3
JUMP95_MAX = 0.45

def crossings(path):
    w, h, buf = decode(path)
    grid = [[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * 4
            grid[y][x] = (0.2126 * buf[idx] + 0.7152 * buf[idx+1] + 0.0722 * buf[idx+2]) / 255.0

    cand = []
    for y in range(h):
        r = grid[y]
        lo, hi = min(r), max(r)
        out = []
        if hi - lo >= .15:
            mid = .5 * (lo + hi)
            for x0 in range(w - 1):
                if (r[x0] < mid) != (r[x0+1] < mid):
                    r0, r1 = r[x0], r[x0+1]
                    if abs(r1 - r0) > 1e-6:
                        out.append(x0 + (mid - r0) / (r1 - r0))
        cand.append(out)

    xe = [None] * h
    exp = [y * (w - 1) / max(h - 1, 1) for y in range(h)]
    besty, bestd = -1, 1e9
    for y in range(h):
        for v in cand[y]:
            d0 = abs(v - exp[y])
            if d0 < bestd:
                bestd, besty = d0, y
    if besty < 0:
        return xe
    xe[besty] = min(cand[besty], key=lambda v: abs(v - exp[besty]))
    for y in range(besty + 1, h):
        if cand[y]:
            xe[y] = min(cand[y], key=lambda v: abs(v - xe[y - 1]))
    for y in range(besty - 1, -1, -1):
        if cand[y]:
            xe[y] = min(cand[y], key=lambda v: abs(v - xe[y + 1]))
    return xe

def metrics(path):
    xe = crossings(path)
    h = len(xe)
    m = int(h * .06)
    valid_pts = [(y, xe[y]) for y in range(m, h - m) if xe[y] is not None]
    if len(valid_pts) < h * 0.55:
        return None
    
    ys = [p[0] for p in valid_pts]
    xx = [p[1] for p in valid_pts]
    
    # Simple linear fit
    n = len(ys)
    mean_y = sum(ys) / n
    mean_x = sum(xx) / n
    num = sum((ys[i] - mean_y) * (xx[i] - mean_x) for i in range(n))
    den = sum((ys[i] - mean_y)**2 for i in range(n)) or 1e-6
    slope = num / den
    intercept = mean_x - slope * mean_y
    
    res = [xx[i] - (slope * ys[i] + intercept) for i in range(n)]
    d = [abs(res[i] - res[i-1]) for i in range(1, n)]
    d_sorted = sorted(d)
    j95 = d_sorted[int(0.95 * len(d_sorted))] if d else 0.0
    
    run = best = 0
    for i in range(1, n):
        if ys[i] == ys[i - 1] + 1 and abs(xx[i] - xx[i - 1]) < .06:
            run += 1
        else:
            best = max(best, run)
            run = 0
    best = max(best, run) + 1
    return best, j95, max(d) if d else 0.0, len(ys), h

def main():
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        jobs = [("ship2x", 2, SHIP2, True),
                ("ship4x", 4, SHIP4, True),
                ("probe-crisp", 4, PROBE, False)]
        for name, scale, recipe, must_pass in jobs:
            out = os.path.join(td, f"{name}.webp")
            r = subprocess.run([LAB, SRC, out, str(scale), *recipe], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL {name}: celup_lab exited {r.returncode}")
                fails += 1
                continue
            m = metrics(out)
            if m is None:
                print(f"FAIL {name}: no usable edge crossings")
                fails += 1
                continue
            best, j95, jmax, rows, h = m
            ok = best <= TREADRUN_MAX and j95 <= JUMP95_MAX
            desc = f"treadrun={best} (<= {TREADRUN_MAX})  jump95={j95:.3f} (<= {JUMP95_MAX})"
            if must_pass:
                print(f"{'ok  ' if ok else 'FAIL'} {name}: {desc}")
                if not ok: fails += 1
            else:
                print(f"{'ok  ' if not ok else 'FAIL'} {name}: {desc}  "
                      f"({'staircase flagged as expected' if not ok else 'NOT flagged -- detector toothless'})")
                if ok: fails += 1
    print("PASS" if fails == 0 else f"{fails} FAILURES")
    return 0 if fails == 0 else 1

if __name__ == "__main__":
    sys.exit(main())

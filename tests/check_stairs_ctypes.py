#!/usr/bin/env python3
"""v4.9.1 staircase gate using Python stdlib + libwebp."""
import ctypes, subprocess, sys, tempfile, os

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

def linfit(ys, xx):
    n = len(ys)
    if n < 2:
        return 0.0, 0.0
    my = sum(ys) / n
    mx = sum(xx) / n
    num = sum((ys[i] - my) * (xx[i] - mx) for i in range(n))
    den = sum((ys[i] - my)**2 for i in range(n)) or 1e-6
    slope = num / den
    intercept = mx - slope * my
    return slope, intercept

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
    pts = [(y, xe[y]) for y in range(m, h - m) if xe[y] is not None]
    if len(pts) < h * 0.55:
        return None
    
    ys = [p[0] for p in pts]
    xx = [p[1] for p in pts]
    
    for _ in range(2):
        slope, intercept = linfit(ys, xx)
        inl_pts = [(y, x) for y, x in zip(ys, xx) if abs(x - (slope * y + intercept)) < 2.0]
        if len(inl_pts) > 8:
            ys = [p[0] for p in inl_pts]
            xx = [p[1] for p in inl_pts]
            
    slope, intercept = linfit(ys, xx)
    
    # Segment extraction
    seg, cur = [], []
    for y, x in zip(ys, xx):
        v = abs(x - (slope * y + intercept)) < 2.0
        if v and (not cur or y - cur[-1][0] <= 3):
            cur.append((y, x))
        else:
            if len(cur) > len(seg):
                seg = cur
            cur = [(y, x)] if v else []
    if len(cur) > len(seg):
        seg = cur
        
    if len(seg) < h * 0.55 * (1 - 2 * .06):
        return None
        
    ys2 = [p[0] for p in seg]
    xx2 = [p[1] for p in seg]
    s2, i2 = linfit(ys2, xx2)
    res = [xx2[idx] - (s2 * ys2[idx] + i2) for idx in range(len(ys2))]
    d = [abs(res[idx] - res[idx-1]) for idx in range(1, len(res))]
    d_sorted = sorted(d)
    j95 = d_sorted[int(0.95 * len(d_sorted))] if d else 0.0
    
    run = best = 0
    for idx in range(1, len(ys2)):
        if ys2[idx] == ys2[idx - 1] + 1 and abs(xx2[idx] - xx2[idx - 1]) < .06:
            run += 1
        else:
            best = max(best, run)
            run = 0
    best = max(best, run) + 1
    return best, j95, max(d) if d else 0.0, len(ys2), h

def verdict(m):
    tread, j95, jmax, rows, h = m
    ok = tread <= TREADRUN_MAX and j95 <= JUMP95_MAX
    return ok, f"treadrun={tread} (<= {TREADRUN_MAX})  jump95={j95:.3f} (<= {JUMP95_MAX})  jumpmax={jmax:.3f}  rows={rows}/{h}"

def main():
    if not os.path.exists(SRC) or not os.path.exists(LAB):
        return 1
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        jobs = [("ship2x", 2, SHIP2, True),
                ("ship4x", 4, SHIP4, True),
                ("probe-crisp", 4, PROBE, False)]
        for name, scale, recipe, must_pass in jobs:
            out = os.path.join(td, f"{name}.webp")
            r = subprocess.run([LAB, SRC, out, str(scale), *recipe], capture_output=True, text=True)
            if r.returncode != 0:
                fails += 1
                continue
            m = metrics(out)
            if m is None:
                fails += 1
                continue
            ok, desc = verdict(m)
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

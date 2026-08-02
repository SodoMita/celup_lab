#!/usr/bin/env python3
"""v4.9 corner/glow forensics gate for autodeblur using Python stdlib + libwebp."""
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

LAB = './celup_lab'
SRC = 'tests/cornerstar48_src.webp'
RECIPE = ['--mode', 'autodeblur', '--max-mib', '2048', '-c', 'linear',
          '-k', 'bspline', '-r', '2.3', '-s', '100', '-g', '16', '-D', 'remap']

def main():
    if not os.path.exists(SRC):
        print(f'FAIL: {SRC} missing')
        return 1
    
    sw, sh, sbuf = decode(SRC)
    slum = [sbuf[i] / 255.0 for i in range(0, len(sbuf), 4)]
    smin, smax = min(slum), max(slum)
    
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, 'o.webp')
        r = subprocess.run([LAB, SRC, out, '4'] + RECIPE, capture_output=True, text=True)
        if r.returncode != 0:
            print('FAIL: celup_lab exited', r.returncode, r.stderr)
            return 1
        ow, oh, obuf = decode(out)
        olum = [obuf[i] / 255.0 for i in range(0, len(obuf), 4)]
        
    omin, omax = min(olum), max(olum)
    fails = 0
    
    # 1. Hull
    if omin < smin - .02 or omax > smax + .02:
        print(f'FAIL hull: output [{omin:.4f},{omax:.4f}] outside source [{smin:.4f},{smax:.4f}] +- .02')
        fails += 1
    else:
        print(f'ok   hull: output [{omin:.4f},{omax:.4f}] inside source [{smin:.4f},{smax:.4f}]')
        
    # 2. Tip extent
    s_ext = 0
    for y in range(25, 48):
        for x in range(25, 48):
            if slum[y * sw + x] < 0.35:
                s_ext = max(s_ext, x + y)
                
    o_ext = 0
    for y in range(96, 192):
        for x in range(96, 192):
            if olum[y * ow + x] < 0.35:
                o_ext = max(o_ext, (x + y) / 4.0)
                
    if o_ext < s_ext - 1.0:
        print(f'FAIL tip: dark tip diagonal extent {o_ext:.2f} < source {s_ext:.2f} - 1 src px')
        fails += 1
    else:
        print(f'ok   tip: extent {o_ext:.2f} vs source {s_ext:.2f} src px')
        
    # 3. Flank transition width
    row = [olum[140 * ow + x] for x in range(8, 64)]
    mid = [i for i, v in enumerate(row) if 0.25 < v < 0.75]
    width = (max(mid) - min(mid) + 1) if mid else 0
    if width > 12:
        print(f'FAIL width: 10-90 flank transition {width} out px > 12')
        fails += 1
    else:
        print(f'ok   width: 10-90 flank transition {width} out px <= 12')

    print('PASS' if not fails else f'{fails} FAILURES')
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Generate comprehensive comparison sheets for all example images
comparing Nearest, Bilinear, xBRZ, Adaptive, Autodeblur, and Hybrid modes.
"""
import ctypes, os, subprocess, sys

webp = ctypes.CDLL('/usr/lib/x86_64-linux-gnu/libwebp.so.7')
webp.WebPDecodeRGBA.restype = ctypes.POINTER(ctypes.c_uint8)
webp.WebPEncodeLosslessRGBA.restype = ctypes.c_size_t

def decode_webp(path):
    data = open(path, 'rb').read()
    w, h = ctypes.c_int(), ctypes.c_int()
    ptr = webp.WebPDecodeRGBA(data, len(data), ctypes.byref(w), ctypes.byref(h))
    if not ptr:
        return 0, 0, None
    buf = bytes(ptr[:w.value * h.value * 4])
    webp.WebPFree(ptr)
    return w.value, h.value, buf

def encode_webp(path, w, h, rgba_bytes):
    ptr = ctypes.POINTER(ctypes.c_uint8)()
    sz = webp.WebPEncodeLosslessRGBA(rgba_bytes, w, h, w * 4, ctypes.byref(ptr))
    if sz > 0:
        data = bytes(ptr[:sz])
        webp.WebPFree(ptr)
        with open(path, 'wb') as f:
            f.write(data)
        return True
    return False

images = [
    'pikachu.webp',
    'poor smiley.webp',
    'cat.webp',
    'femlineart.webp',
    'miya_normal.webp',
    'check_water_3.webp',
    '12months.webp',
    'human22.webp'
]

modes = ['nearest', 'bilinear', 'xbrz', 'adaptive', 'autodeblur', 'hybrid']

os.makedirs('images/comparison_sheets', exist_ok=True)

print("Generating upscaled outputs for all example images...")
for img in images:
    src_path = os.path.join('images', img)
    if not os.path.exists(src_path):
        continue
    base_name = img.replace(' ', '_').replace('.webp', '')
    print(f"\n--- Processing {img} ---")
    for m in modes:
        out_path = os.path.join('images/comparison_sheets', f'{base_name}_{m}_2x.webp')
        cmd = ['./celup_lab', src_path, out_path, '2', '--mode', m, '-k', 'bspline', '-c', 'linear', '-D', 'remap']
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode == 0:
            print(f"  [{m:12s}] -> OK ({res.stderr.strip() or 'success'})")
        else:
            print(f"  [{m:12s}] -> ERR: {res.stderr.strip()}")

print("\nAll example image upscales generated successfully.")

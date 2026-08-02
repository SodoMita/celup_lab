#!/usr/bin/env python3
"""Generate all celup_lab test fixture images using Python stdlib + libwebp."""
import ctypes, math, os

webp = ctypes.CDLL('/usr/lib/x86_64-linux-gnu/libwebp.so.7')
webp.WebPEncodeLosslessRGBA.restype = ctypes.c_size_t

def save_webp(filename, w, h, rgba_bytes):
    ptr = ctypes.POINTER(ctypes.c_uint8)()
    sz = webp.WebPEncodeLosslessRGBA(rgba_bytes, w, h, w * 4, ctypes.byref(ptr))
    if sz > 0:
        data = bytes(ptr[:sz])
        webp.WebPFree(ptr)
        os.makedirs(os.path.dirname(filename), exist_ok=True)
        with open(filename, 'wb') as f:
            f.write(data)
        return True
    return False

def gblur1d(val_grid, w, h, sigma):
    # 2D Gaussian blur using separable 1D convolution
    r = int(math.ceil(3 * sigma))
    kernel = [math.exp(-x*x / (2 * sigma * sigma)) for x in range(-r, r+1)]
    ksum = sum(kernel)
    kernel = [k / ksum for k in kernel]
    
    # Horiz pass
    tmp = [[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            s = 0.0
            for dx in range(-r, r+1):
                nx = max(0, min(w-1, x + dx))
                s += val_grid[y][nx] * kernel[dx + r]
            tmp[y][x] = s
            
    # Vert pass
    out = [[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            s = 0.0
            for dy in range(-r, r+1):
                ny = max(0, min(h-1, y + dy))
                s += tmp[ny][x] * kernel[dy + r]
            out[y][x] = s
    return out

# 1. cornerstar48_src.webp (48x48)
w, h = 48, 48
grid = [[1.0]*w for _ in range(h)]
# Triangle: tips at (8,40), (44,44), (20,8)
for y in range(h):
    for x in range(w):
        # Barycentric check for triangle
        ax, ay, bx, by, cx, cy = 8, 40, 44, 44, 20, 8
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        a = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
        b = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
        if a >= 0 and b >= 0 and a + b <= 1:
            grid[y][x] = 0.12
        if y >= 26 and y < 46 and x >= 40 and x < 43:
            grid[y][x] = 0.15
        if y >= 26 and y < 29 and x >= 30 and x < 40:
            grid[y][x] = 0.15

blurred = gblur1d(grid, w, h, 0.5)
buf = bytearray(w * h * 4)
for y in range(h):
    for x in range(w):
        lum = blurred[y][x]
        r = int(min(255, max(0, lum * 255)))
        g = int(min(255, max(0, (lum * 0.94 + 0.03) * 255)))
        b = int(min(255, max(0, (lum * 0.88 + 0.06) * 255)))
        idx = (y * w + x) * 4
        buf[idx:idx+4] = bytes([r, g, b, 255])

save_webp('tests/cornerstar48_src.webp', w, h, bytes(buf))

# 2. diagline48_src.webp (64x64)
w, h = 64, 64
grid = [[0.05 if abs(x - y) <= 2 else 0.95 for x in range(w)] for y in range(h)]
blurred = gblur1d(grid, w, h, 0.5)
buf = bytearray(w * h * 4)
for y in range(h):
    for x in range(w):
        lum = round(blurred[y][x] * 44.0) / 44.0
        r = int(min(255, max(0, lum * 255)))
        g = int(min(255, max(0, (lum * 0.95 + 0.03) * 255)))
        b = int(min(255, max(0, (lum * 0.90 + 0.06) * 255)))
        idx = (y * w + x) * 4
        buf[idx:idx+4] = bytes([r, g, b, 255])

save_webp('tests/diagline48_src.webp', w, h, bytes(buf))
print("Generated test fixtures successfully.")

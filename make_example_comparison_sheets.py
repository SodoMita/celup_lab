#!/usr/bin/env python3
"""Generate comparison sheets for all example images in images/, comparing the
key upscale modes.  Inherited from the other agent branches' tool and made
robust to the webp library location (system libwebp.so.7 OR the /tmp/webpshim
copy described in ARENA_AGENTS.md).

Usage:  python3 make_example_comparison_sheets.py [SCALE] [extra celup_lab flags...]

By default it runs nearest/bilinear/adaptive/autodeblur at SCALE=2 on every
*.webp in images/ and writes images/comparison_sheets/<base>_<mode>_<scale>x.webp
plus a text summary to comparison_sheets_examples.md.
"""
import ctypes, glob, os, subprocess, sys

LIB_CANDIDATES = [
    '/usr/lib/x86_64-linux-gnu/libwebp.so.7',
    '/tmp/webpshim/libwebp.so.7',
    '/tmp/webpshim/libwebp.so',
]
webp = None
for _lib in LIB_CANDIDATES:
    try:
        webp = ctypes.CDLL(_lib)
        break
    except OSError:
        continue
if webp is None:
    sys.exit('ERROR: cannot find libwebp (tried %s)' % LIB_CANDIDATES)
webp.WebPDecodeRGBA.restype = ctypes.POINTER(ctypes.c_uint8)
webp.WebPEncodeLosslessRGBA.restype = ctypes.c_size_t

LAB = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'celup_lab')
IMG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'images')
OUT_DIR = os.path.join(IMG_DIR, 'comparison_sheets')


def decode_webp(path):
    data = open(path, 'rb').read()
    w, h = ctypes.c_int(), ctypes.c_int()
    ptr = webp.WebPDecodeRGBA(data, len(data), ctypes.byref(w), ctypes.byref(h))
    if not ptr:
        return 0, 0, None
    buf = bytes(ptr[:w.value * h.value * 4])
    webp.WebPFree(ptr)
    return w.value, h.value, buf


def dims(path):
    w, h, _ = decode_webp(path)
    return w, h


def main():
    scale = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].isdigit() else '2'
    extra = [a for a in sys.argv[2:] if not a.isdigit()]
    modes = ['nearest', 'bilinear', 'adaptive', 'autodeblur']
    os.makedirs(OUT_DIR, exist_ok=True)
    images = sorted(glob.glob(os.path.join(IMG_DIR, '*.webp')))
    if not images:
        print('no *.webp in %s -- nothing to do' % IMG_DIR)
        return 0
    print('Generating upscaled outputs for %d example images at %sx...' %
          (len(images), scale))
    summary = ['# example comparison sheets (scale %sx)' % scale, '',
               '| image | mode | in | out | status |', '|---|---|---|---|---|']
    for img in images:
        base = os.path.splitext(os.path.basename(img))[0].replace(' ', '_')
        iw, ih = dims(img)
        print('\n--- %s (%dx%d) ---' % (os.path.basename(img), iw, ih))
        for m in modes:
            out_path = os.path.join(OUT_DIR, '%s_%s_%sx.webp' % (base, m, scale))
            cmd = [LAB, img, out_path, scale, '--mode', m, '-M', '2048']
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode == 0:
                ow, oh = dims(out_path)
                line = 'done'
                print('  [%-11s] -> OK  %dx%d' % (m, ow, oh))
                summary.append('| %s | %s | %dx%d | %dx%d | OK |' %
                               (base, m, iw, ih, ow, oh))
            else:
                print('  [%-11s] -> ERR: %s' % (m, res.stderr.strip()[:120]))
                summary.append('| %s | %s | %dx%d | - | ERR |' %
                               (base, m, iw, ih))
    out_md = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          'comparison_sheets_examples.md')
    open(out_md, 'w').write('\n'.join(summary) + '\n')
    print('\nWrote', out_md)
    print('Outputs in', OUT_DIR)
    return 0


if __name__ == '__main__':
    sys.exit(main())

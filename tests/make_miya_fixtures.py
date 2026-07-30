#!/usr/bin/env python3
"""Derive the miya face-crop test fixture from the user-supplied asset.

The miya artwork itself is a user upload and is NOT kept in git (no image
files in the repository).  If tests/miya_normal.webp is present (copy the
user-provided miya_normal.webp there), this writes tests/miya_face.webp,
the 320x300 face crop used by tests/test_scales.py and review sheets.
Otherwise it exits quietly with a note -- all other fixtures come from
tests/make_test_sources.py and need no external assets.
"""
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
src = HERE / 'miya_normal.webp'
dst = HERE / 'miya_face.webp'
if not src.exists():
    print('tests/miya_normal.webp not found; skipping (user asset, not in git)')
    raise SystemExit(0)
im = Image.open(src).convert('RGBA')
im.crop((240, 280, 560, 580)).save(dst, lossless=True)
print('wrote', dst)
# 32x11 strip across the left eye edge/hair boundary: enough content to see
# upscale artifacts at extreme scale (22x -> 704x242) without heavy renders.
strip = HERE / 'miya_face32x11.webp'
im.crop((330, 436, 362, 447)).save(strip, lossless=True)
print('wrote', strip)

#!/usr/bin/env python3
"""Create the user's test images procedurally since uploads are unavailable."""
import numpy as np
from PIL import Image

def make_poor_smiley():
    """256x256 hard pixelated smiley face -- the user's test image.
    Black outlines on white, with simple fill regions."""
    size = 256
    img = np.full((size, size, 4), 255, dtype=np.uint8)  # white bg
    
    # Draw smiley outline (circle-ish, blocky)
    cy, cx = 128, 128
    r = 100
    for y in range(size):
        for x in range(size):
            d = np.sqrt((x-cx)**2 + (y-cy)**2)
            # Outer ring (face border), thick 3px
            if 95 <= d <= 100:
                img[y, x] = [0, 0, 0, 255]
            # Eyes
            elif 70 <= d <= 95 and 60 <= x <= 100 and 70 <= y <= 100:
                img[y, x] = [0, 0, 0, 255]
            elif 70 <= d <= 95 and 155 <= x <= 195 and 70 <= y <= 100:
                img[y, x] = [0, 0, 0, 255]
            # Mouth (smile arc)
            elif 110 <= d <= 115 and 80 <= x <= 175 and 130 <= y <= 175:
                img[y, x] = [0, 0, 0, 255]
            # Cheek blush (pink, semi-transparent-ish)
            elif 85 <= d <= 92 and 50 <= x <= 75 and 110 <= y <= 130:
                img[y, x] = [255, 180, 180, 200]
            elif 85 <= d <= 92 and 180 <= x <= 205 and 110 <= y <= 130:
                img[y, x] = [255, 180, 180, 200]
    
    Image.fromarray(img, "RGBA").save("tests/poor_smiley.webp", lossless=True)
    print("wrote tests/poor_smiley.webp")

def make_miya_facehalf():
    """Anime-style face half -- gradients, skin tones, eye, hair.
    128x128 to mimic a cropped character face."""
    size = 128
    img = np.zeros((size, size, 4), dtype=np.uint8)
    
    # Skin base gradient (top-left lighter, bottom-right darker)
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2 * size)
            r = int(240 - 40 * t)
            g = int(210 - 50 * t)
            b = int(195 - 55 * t)
            img[y, x] = [r, g, b, 255]
    
    # Hair (dark purple, top portion)
    for y in range(40):
        for x in range(size):
            # Wavy hairline
            wave = int(5 * np.sin(x * 0.15))
            if y < 30 + wave:
                img[y, x] = [40, 20, 60, 255]
            elif y < 35 + wave:
                # Hair-skin transition
                blend = (y - 30 - wave) / 5
                img[y, x] = [int(40 + (240-40)*blend*0.3), 
                            int(20 + (210-20)*blend*0.3),
                            int(60 + (195-60)*blend*0.3), 255]
    
    # Eye (large anime eye, right side)
    ex, ey, er = 85, 60, 18
    for y in range(size):
        for x in range(size):
            d = np.sqrt((x-ex)**2 + (y-ey)**2)
            if d < er:
                # Iris gradient
                t = d / er
                img[y, x] = [int(100 + 80*t), int(50 + 60*t), int(180 - 40*t), 255]
            elif d < er + 3:
                # Eye outline
                img[y, x] = [20, 10, 30, 255]
    
    # Eyebrow
    for y in range(35, 42):
        for x in range(65, 110):
            img[y, x] = [30, 15, 40, 255]
    
    # Nose hint (subtle shadow)
    for y in range(70, 85):
        for x in range(55, 62):
            t = (y - 70) / 15
            img[y, x] = [int(220 - 30*t), int(190 - 30*t), int(175 - 30*t), 255]
    
    # Mouth
    for y in range(95, 100):
        for x in range(70, 100):
            img[y, x] = [200, 120, 120, 255]
    
    Image.fromarray(img, "RGBA").save("tests/miya_facehalf.webp", lossless=True)
    print("wrote tests/miya_facehalf.webp")

make_poor_smiley()
make_miya_facehalf()

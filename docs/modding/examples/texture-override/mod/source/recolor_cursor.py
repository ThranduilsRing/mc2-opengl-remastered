#!/usr/bin/env python3
"""
recolor_cursor.py -- "hello world" texture-override example for MC2.

Reads the stock mouse-cursor sprite sheet (a copy of the real
data/art/cursors1.tga shipped inside the game's packed art.fst -- see the
tutorial README for how that path was verified) and writes a recolored
version to mod/data/art/cursors1.tga, i.e. the exact relative path a mod
must use to shadow it.

Stock file facts (checked with Pillow before writing this script):
  - 128x128, RGBA
  - Cursors are packed into a grid of 32x32 (or 31x31) cells; unused pixels
    are keyed as magenta with alpha=0 -- (255, 0, 255, 0). See the comment
    in mclib/mouse.cpp: "Cursors must NOT be anything BUT keyed from now on."

The recolor below:
  - Leaves fully-transparent pixels (alpha == 0) completely untouched, so
    the color-key never changes and cursor hotspots/shapes are unaffected.
  - Shifts the hue of every opaque pixel towards green, so every cursor in
    the sheet (walk, run, attack, etc.) reads as an obvious green tint
    in-game instead of the stock white/grey line art.

Usage:
    py -3 recolor_cursor.py

Requires: Pillow (py -3 -m pip install Pillow)
"""
import colorsys
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "cursors1_stock.tga")
DST = os.path.join(HERE, "..", "data", "art", "cursors1.tga")

# How far to rotate hue towards green (0.0-1.0 fraction of the color wheel).
# 0.33 == pure green.
TARGET_HUE = 0.33


def recolor(src_path: str, dst_path: str) -> None:
    im = Image.open(src_path).convert("RGBA")
    px = im.load()
    w, h = im.size

    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue  # leave the color-key/transparent background alone
            hh, ll, ss = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            # Blend the pixel's hue towards TARGET_HUE instead of hard-setting
            # it, so shading/highlights in the original art are preserved.
            new_h = (hh + (TARGET_HUE - hh) * 0.85) % 1.0
            nr, ng, nb = colorsys.hls_to_rgb(new_h, ll, max(ss, 0.55))
            px[x, y] = (int(nr * 255), int(ng * 255), int(nb * 255), a)

    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    # Uncompressed 32-bit TGA -- matches the stock file's format, avoids any
    # RLE-encoder edge cases.
    im.save(dst_path, format="TGA", rle=False)
    print(f"wrote {os.path.abspath(dst_path)} ({im.size[0]}x{im.size[1]}, {im.mode})")


if __name__ == "__main__":
    recolor(SRC, DST)

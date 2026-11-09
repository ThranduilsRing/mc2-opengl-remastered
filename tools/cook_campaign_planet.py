#!/usr/bin/env python3
"""Cook the shared campaign marker atlas from the stock design.

    py -3 tools/cook_campaign_planet.py

The stock TGA has nonzero alpha on black padding; the old PNG export lost
alpha entirely. Black is the atlas background, including the space inside
the concentric triangles. Key it before either renderer samples the atlas.
All other colours and their original coverage remain unchanged.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "art/source/campaign-planet/mcl_cm_5.tga"
OUTPUT = ROOT / "data/art/gui"


def marker_alpha(image):
    """Remove the black key throughout the atlas, including icon interiors."""
    rgba = image.convert("RGBA")
    rgba.putdata([(r, g, b, 0 if r == g == b == 0 else a)
                  for r, g, b, a in rgba.getdata()])
    return rgba


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with Image.open(SOURCE) as source:
        atlas = marker_alpha(source)
    atlas.save(OUTPUT / "mcl_cm_5.png")
    atlas.save(OUTPUT / "mcl_cm_5.tga", compression="tga_rle")
    print("campaign marker atlas: PNG and TGA cooked with transparent black")


if __name__ == "__main__":
    main()

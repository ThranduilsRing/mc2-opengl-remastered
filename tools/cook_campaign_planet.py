#!/usr/bin/env python3
"""Cook the shared campaign marker atlas from the stock design.

    py -3 tools/cook_campaign_planet.py

The stock TGA has nonzero alpha on black padding; the old PNG export lost
alpha entirely. Black is the atlas background, including the space inside
the concentric triangles. Key it before either renderer samples the atlas.
All other colours and their original coverage remain unchanged.
"""
import json
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
    for name in ("mcl_cm_5", "mcl_mn_4"):
        with Image.open(SOURCE.with_name(name + ".tga")) as source:
            atlas = marker_alpha(source)
        atlas.save(OUTPUT / (name + ".png"))
        atlas.save(OUTPUT / (name + ".tga"), compression="tga_rle")
        print(f"{name}: PNG and TGA cooked with transparent black")
    # Eleven registered poses reconstruct one surface; all thirteen operation
    # images are rendered from it. The TGA sibling wins in the existing loader.
    import campaign_globe as globe
    registration = json.loads((globe.SOURCE / 'registration.json').read_text())
    maps = globe.SOURCE / 'maps'
    globe.reconstruct(globe.SOURCE, registration, maps)
    globe.render(globe.SOURCE, registration, maps, OUTPUT, extension='tga')


if __name__ == "__main__":
    main()

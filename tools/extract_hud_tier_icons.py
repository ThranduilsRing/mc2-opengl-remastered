#!/usr/bin/env python3
"""Restore v0.4's hand-pixeled HUD icons as per-draw-size files next to the single source.

The legacy tree shipped data/art/mechicons/hud/mcui_{high7,med4,low4}/{front,back}/<stem>.png
(42x50, 34x40, 27x32) and the HUD drew them into 40x48 / 32x38 / 25x30 cells with nearest sampling
(ForceGroupIcon::setIconVariables). The single-res branch dropped the tiers and box-filtered the
42x50 art, which blurs. unit_icons::blit prefers hud/<side>/<stem>@<W>x<H>.png when the HUD draws
exactly that cell size, so this script writes the v0.4 result for each cell size: the tier art
nearest-sampled to the cell, the same pixels v0.4 drew.

  py -3 tools/extract_hud_tier_icons.py [--ref 7d2b1b189^] [--check]

--check only verifies every stem has every size with the right dimensions (no writes).
"""
import argparse
import io
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
HUD = ROOT / "data" / "art" / "mechicons" / "hud"
# legacy tier -> HUD cell (width, height) it was drawn into
TIERS = {"mcui_high7": (40, 48), "mcui_med4": (32, 38), "mcui_low4": (25, 30)}
DEFAULT_REF = "7d2b1b189^"  # last commit with the tiered tree


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True).stdout


def nearest(img, w, h):
    sw, sh = img.size
    src = img.load()
    out = Image.new("RGBA", (w, h))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            dst[x, y] = src[x * sw // w, y * sh // h]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default=DEFAULT_REF)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    bad = 0
    for side in ("front", "back"):
        stems = sorted(p.stem for p in (HUD / side).glob("*.png") if "@" not in p.stem)
        for tier, (w, h) in TIERS.items():
            for stem in stems:
                dst = HUD / side / f"{stem}@{w}x{h}.png"
                if a.check:
                    if not dst.is_file() or Image.open(dst).size != (w, h):
                        print(f"MISSING/WRONG {dst.relative_to(ROOT)}")
                        bad += 1
                    continue
                data = git("show", f"{a.ref}:data/art/mechicons/hud/{tier}/{side}/{stem}.png")
                nearest(Image.open(io.BytesIO(data)).convert("RGBA"), w, h).save(dst)
        print(f"{side}: {len(stems)} stems x {len(TIERS)} sizes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Restore v0.4's hand-pixeled HUD icons as per-draw-size files next to the single source.

v0.4 drew the cells of the legacy sheets mcui_{high7,med4,low4}.tga (40x48 / 32x38 / 25x30, see
unit_icons::init addSheet) straight into the HUD cell. The old split tree stored each cell with a 1 px
transparent border (hud/mcui_*/front|back/<stem>.png at 42x50 / 34x40 / 27x32); the single-res branch
kept only the 42x50 one and box-filtered it down, which blurs. unit_icons::blit prefers
hud/<side>/<stem>@<W>x<H>.png when the HUD draws exactly that cell size, so this script writes each
tier's cell (border cropped off), byte-identical in pixels to the v0.4 sheet cell.

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
sys.path.append(str(ROOT / "scripts"))
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)
HUD = ROOT / "data" / "art" / "mechicons" / "hud"
# legacy tier -> HUD cell (width, height); the tier PNG is the cell plus a 1 px transparent border
TIERS = {"mcui_high7": (40, 48), "mcui_med4": (32, 38), "mcui_low4": (25, 30)}
DEFAULT_REF = "7d2b1b189^"  # last commit with the tiered tree


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True,
                          **console_child.child_kwargs(redirected=True)).stdout


def crop_cell(img, w, h):
    assert img.size == (w + 2, h + 2), (img.size, w, h)
    cell = img.crop((1, 1, w + 1, h + 1))
    frame = img.copy()
    frame.paste((0, 0, 0, 0), (1, 1, w + 1, h + 1))
    assert frame.getchannel("A").getextrema()[1] == 0, "tier border is not transparent"
    return cell


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
                crop_cell(Image.open(io.BytesIO(data)).convert("RGBA"), w, h).save(dst)
        print(f"{side}: {len(stems)} stems x {len(TIERS)} sizes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

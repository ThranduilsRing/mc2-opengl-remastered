#!/usr/bin/env python3
"""Restore the hand-pixeled HUD icon tiers as per-size files next to the single source.

The legacy tree shipped data/art/mechicons/hud/mcui_{med4,low4}/{front,back}/<stem>.png. The
single-res branch dropped them and box-filtered the 42x50 art, which blurs. unit_icons::blit prefers
hud/<side>/<stem>@<W>x<H>.png when the HUD draws exactly that size.

  py -3 tools/extract_hud_tier_icons.py [--ref adc4b73fc^] [--check]

--check only verifies every stem has every size (no writes). Files are copied byte for byte.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HUD = ROOT / "data" / "art" / "mechicons" / "hud"
TIERS = {"mcui_med4": (34, 40), "mcui_low4": (27, 32)}
DEFAULT_REF = "7d2b1b189^"  # last commit with the tiered tree


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True).stdout


def png_size(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    return struct.unpack(">II", data[16:24])


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
                    if not dst.is_file() or png_size(dst.read_bytes()) != (w, h):
                        print(f"MISSING/WRONG {dst.relative_to(ROOT)}")
                        bad += 1
                    continue
                data = git("show", f"{a.ref}:data/art/mechicons/hud/{tier}/{side}/{stem}.png")
                if png_size(data) != (w, h):
                    print(f"size mismatch {tier}/{side}/{stem}: {png_size(data)}")
                    bad += 1
                    continue
                dst.write_bytes(data)
        print(f"{side}: {len(stems)} stems x {len(TIERS)} sizes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

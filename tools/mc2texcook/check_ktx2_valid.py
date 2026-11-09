#!/usr/bin/env python3
"""ktx2_valid contract row: `ktx validate` over a fresh cook fixture (tracked tests/fixtures ktx2 are deliberately malformed, so not swept).

  py -3 tools/mc2texcook/check_ktx2_valid.py [--install DIR]   (DIR: sweep every *.ktx2 under it)

Fails closed when the ktx tool (KTX-Software 4.4.2) is missing.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mc2texcook"))
DEFAULT_KTX = "A:/Games/mc2-tools/ktx/ktx.exe"
INSTALL_HELP = ("ktx (KTX-Software v4.4.2) not found. Install the release from "
                "https://github.com/KhronosGroup/KTX-Software/releases/tag/v4.4.2 to "
                f"{DEFAULT_KTX} (Windows) or put `ktx` on PATH / set KTX_TOOL (Linux: KTX-Software-4.4.2-Linux-x86_64).")


def find_ktx() -> str | None:
    for c in (os.environ.get("KTX_TOOL"), DEFAULT_KTX):
        if c and Path(c).is_file():
            return c
    return shutil.which("ktx")


def fixture_files(tmp: Path) -> list[Path]:
    import mc2texcook
    from PIL import Image
    out = []
    for preset in ("albedo", "normal", "orm", "mask"):
        rgba, vk = mc2texcook._apply_preset(Image.new("RGBA", (16, 8), (200, 100, 50, 255)), preset)
        p = tmp / f"fixture_{preset}.ktx2"
        mc2texcook.write_ktx2(p, mc2texcook._generate_mips(rgba), vk)
        out.append(p)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", help="also validate every *.ktx2 under this directory")
    args = ap.parse_args()
    ktx = find_ktx()
    if not ktx:
        # 77 = HOST_INFRA: check-contracts.sh fails it on the PC and reports it INFRA (never PASS) only under MC2_HOST_INFRA_OK=1
        print("HOST_INFRA ktx2_valid: " + INSTALL_HELP)
        return 77
    files: list[Path] = []
    if args.install:
        files += sorted(Path(args.install).rglob("*.ktx2"))
    bad = 0
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMP") or None) as td:
        files += fixture_files(Path(td))
        for f in files:
            r = subprocess.run([ktx, "validate", "--format", "mini-json", str(f)], capture_output=True, text=True)
            if r.returncode != 0:
                bad += 1
                print(f"INVALID {f}: {(r.stdout or r.stderr).strip()[:300]}")
    print(f"ktx2_valid: {len(files) - bad}/{len(files)} valid")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Stack each chassis' *_weapons atlas under its base atlas in an installed BT2018 pack.

The engine gives an imported mech one skin slot, so a part using the second (weapons) material sampled the
base atlas with weapons UVs (garbled shoulder autocannon on the Hunchback). This script writes stacked
albedo / AO / normal textures (base in the top half, weapons resampled into the bottom half), re-cooks them
to BC7 .ktx2 under the names the package already declares, and marks the package with "atlasStack" so the
importer (mclib/assimp_importer.cpp PopulateMergedSkinnedShape) remaps weapon-material UVs into the lower half.

  py -3 tools/bt2018_mechs/stack_weapons_atlas.py --pack <pack mod dir> --dump <BattleTech_2018_Dump> \
      [--ktx A:/Games/mc2-tools/ktx/ktx.exe] [--only hunchback,crab]

Chassis whose dump has no weapons albedo, or whose package is already marked, are skipped. Idempotent.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bt2018_import"))
from cook_ao_from_package import preprocess_unity_normal  # noqa: E402

DEFAULT_KTX = r"A:/Games/mc2-tools/ktx/ktx.exe"


def stack(base: Image.Image, weapons: Image.Image) -> Image.Image:
    base = base.convert("RGBA")
    out = Image.new("RGBA", (base.width, base.height * 2))
    out.paste(base, (0, 0))
    out.paste(weapons.convert("RGBA").resize(base.size, Image.LANCZOS), (0, base.height))
    return out


def cook(ktx, png, dst, tmp):
    uastc = Path(tmp) / (dst.name + ".uastc.ktx2")
    r1 = subprocess.run([ktx, "create", "--encode", "uastc", "--format", "R8G8B8A8_UNORM", "--assign-tf", "linear",
                         "--levels", "1", str(png), str(uastc)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    r2 = subprocess.run([ktx, "transcode", "--target", "bc7", str(uastc), str(dst)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) if r1.returncode == 0 else r1
    if r1.returncode or r2.returncode:
        sys.exit(f"ktx failed for {dst.name}: {(r1.stdout + r2.stdout)[:200]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pack", required=True, help="installed pack mod dir (contains data/tgl)")
    ap.add_argument("--dump", required=True, help="BattleTech_2018_Dump root (Texture2D/)")
    ap.add_argument("--ktx", default=DEFAULT_KTX)
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    tgl = Path(args.pack) / "data" / "tgl"
    tex = Path(args.dump) / "Texture2D"
    only = {s.strip().lower() for s in args.only.split(",") if s.strip()}
    done = 0
    for pkg_path in sorted((tgl / "bt2018").glob("*.package.json")):
        chassis = pkg_path.name[:-len(".package.json")]
        if only and chassis not in only:
            continue
        text = pkg_path.read_text(encoding="utf-8")
        if '"atlasStack"' in text:
            continue
        maps = {}
        for key, tag in (("albedo", "alb"), ("ao", "amb"), ("normal", "nrm")):
            b, w = tex / f"chrTxrMech_{chassis}-base-{tag}.png", tex / f"chrTxrMech_{chassis}-weapons-{tag}.png"
            if tag == "alb" and not (b.is_file() and w.is_file()):
                maps = None
                break
            maps[key] = (b, w)
        if not maps:
            continue
        pkg = json.loads(text)
        with tempfile.TemporaryDirectory() as tmp:
            for key, (b, w) in maps.items():
                if not b.is_file():
                    continue
                mat = pkg["materials"].get(key)
                if not mat:
                    continue
                if key == "normal":
                    preprocess_unity_normal(b, Path(tmp) / "b.png")
                    bi = Image.open(Path(tmp) / "b.png")
                    if w.is_file():
                        preprocess_unity_normal(w, Path(tmp) / "w.png")
                        wi = Image.open(Path(tmp) / "w.png")
                    else:
                        wi = Image.new("RGB", bi.size, (128, 128, 255))
                else:
                    bi = Image.open(b)
                    wi = Image.open(w) if w.is_file() else Image.new("RGBA", bi.size, (255, 255, 255, 255))
                stacked = stack(bi, wi)
                png = Path(tmp) / f"{key}.png"
                stacked.save(png)
                cook(args.ktx, png, tgl / mat["ktx2"], tmp)
                if key == "albedo" and (tgl / mat["tga"]).is_file():
                    stacked.save(tgl / mat["tga"])   # the loose TGA outranks a sidecar in places: keep both stacked
        pkg_path.write_text(text.replace('"materials": {', '"materials": {\n    "atlasStack": "weapons",', 1), encoding="utf-8")
        print(f"[stack] {chassis}: stacked weapons atlas into {', '.join(maps)}")
        done += 1
    print(f"[stack] done: {done} chassis")


if __name__ == "__main__":
    main()

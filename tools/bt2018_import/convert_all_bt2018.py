#!/usr/bin/env python3
"""End-to-end BT2018 mech import: convert EVERY mech FBX into a deployable,
clip-retargeted, textured GLB under MC2Conversions/ using ORIGINAL BattleTech names.

Per mech:
  1. FBX2glTF -b                          -> MC2Conversions/<btname>.glb (embeds textures)
  2. retarget clips (atlas reference)     -> in place (legs animate correctly)
  3. extract base-albedo PNG -> TGA       -> MC2Conversions/<btname>/<derived>.tga
  4. deploy GLB + TGA                      -> <deploy data/tgl>/

The engine derives the runtime tga name from the GLB material image via deriveName
(lowercase stem + .tga); file.cpp strips the size subdir so a single data/tgl copy
serves the 128/ request too (no LOD copy needed).

Source FBX never modified. Atlas reference GLB (raw bind) never modified.

Usage:
  py -3 convert_all_bt2018.py \
    --fbx2gltf C:/Users/Joe/Downloads/FBX2glTF-windows-x64.exe \
    --dump     A:/Games/mc2-opengl/BattleTech_2018_Dump \
    --out      A:/Games/mc2-opengl/MC2Conversions \
    --deploy   A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0/data/tgl \
    [--force] [--only marauder,warhammer]
"""
import argparse
import glob
import io
import json
import os
import re
import shutil
import subprocess
import sys

from PIL import Image
from pygltflib import GLTF2

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
from bt2018_retarget_clips import retarget_file  # noqa: E402
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

FBX_GLOB = "Animator/chrPrfMech_*Base-001/chrPrfMech_*Base-001.fbx"
BTNAME_RE = re.compile(r"chrPrfMech_(.+)Base-001", re.IGNORECASE)


def btname_of(fbx_path):
    stem = os.path.splitext(os.path.basename(fbx_path))[0]
    m = BTNAME_RE.match(stem)
    return m.group(1) if m else stem


def image_bytes(gltf, img):
    """Return raw bytes of a glTF image (bufferView-backed; GLB embed)."""
    bv = gltf.bufferViews[img.bufferView]
    blob = gltf.binary_blob()
    off = bv.byteOffset or 0
    return blob[off:off + bv.byteLength]


def pick_albedo(gltf):
    """Pick the base-albedo image: prefer name containing '-base-alb', else any
    '-alb' that isn't a blip, else the largest non-blip/non-nrm image."""
    imgs = gltf.images or []
    def ok(n): return "blip" not in n.lower()
    for want in ("-base-alb", "base-alb", "-alb"):
        for im in imgs:
            if im.name and want in im.name.lower() and ok(im.name):
                return im
    cands = [im for im in imgs if im.name and ok(im.name) and "nrm" not in im.name.lower()]
    return cands[0] if cands else None


def derive_tga_name(png_name):
    """Mirror the engine's deriveName for base albedo: lowercase stem + .tga."""
    stem = os.path.splitext(png_name)[0]
    return stem.lower() + ".tga"


def convert_one(fbx, btname, args):
    out_dir = args.out
    glb = os.path.join(out_dir, f"{btname}.glb")
    asset_dir = os.path.join(out_dir, btname)
    os.makedirs(asset_dir, exist_ok=True)

    # 1. FBX2glTF into a raw intermediate (skip if present unless --force). The cooked GLB is
    #    always derived from the raw one, never re-processed in place: a repeat run gives
    #    identical bytes instead of accumulating the bind correction.
    raw_dir = os.path.join(out_dir, "_fbx2gltf")
    os.makedirs(raw_dir, exist_ok=True)
    raw = os.path.join(raw_dir, f"{btname}.glb")
    if args.force or not os.path.isfile(raw):
        r = subprocess.run([args.fbx2gltf, "-i", fbx, "-o", os.path.join(raw_dir, btname), "-b"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **console_child.child_kwargs(redirected=True))
        if not os.path.isfile(raw):
            print(f"[FAIL] {btname}: FBX2glTF produced no glb\n{r.stdout[-400:]}")
            return None

    # 2. retarget clips raw -> cooked (atlas reference). Atlas itself = identity no-op.
    try:
        retarget_file(raw, args.atlas_ref, glb)
    except Exception as e:
        print(f"[FAIL] {btname}: retarget error: {e}")
        return None

    # 3. extract base-albedo -> TGA
    g = GLTF2().load(glb)
    alb = pick_albedo(g)
    tga_name = None
    if alb is None:
        print(f"[WARN] {btname}: no albedo image found in GLB (skipping texture)")
    else:
        png = image_bytes(g, alb)
        tga_name = derive_tga_name(alb.name)
        tga_path = os.path.join(asset_dir, tga_name)
        Image.open(io.BytesIO(png)).convert("RGBA").save(tga_path)
        shutil.copy2(tga_path, os.path.join(args.deploy, tga_name))

    # 4. deploy GLB
    shutil.copy2(glb, os.path.join(args.deploy, f"{btname}.glb"))
    print(f"[OK] {btname}: glb + tga={tga_name} (anims={len(g.animations or [])})")
    return {"btname": btname, "glb": f"{btname}.glb", "tga": tga_name}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fbx2gltf", required=True)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    args.atlas_ref = os.path.join(args.dump, "Atlas", "atlas_fbx2gltf.glb")
    if not os.path.isfile(args.atlas_ref):
        sys.exit(f"FATAL: atlas reference missing: {args.atlas_ref}")
    os.makedirs(args.out, exist_ok=True)

    only = {s.strip().lower() for s in args.only.split(",") if s.strip()}
    fbxs = sorted(glob.glob(os.path.join(args.dump, FBX_GLOB)))
    manifest = []
    for fbx in fbxs:
        bt = btname_of(fbx)
        if only and bt.lower() not in only:
            continue
        rec = convert_one(fbx, bt, args)
        if rec:
            manifest.append(rec)

    mpath = os.path.join(args.out, "conversions_manifest.json")
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print(f"\n[done] converted {len(manifest)} mechs -> {args.out}")
    print(f"[done] manifest: {mpath}")


if __name__ == "__main__":
    main()

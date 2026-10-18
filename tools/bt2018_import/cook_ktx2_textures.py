#!/usr/bin/env python3
"""KTX2-INFRA-1: cook imported BT mech albedos to FULL-RES BC7 .ktx2 (homogeneous
texture format). Replaces the per-mech .tga albedos. The engine decodes the BC7
sidecar via txmmgr KTX_PRIMARY (default mode 1) when the .tga is absent; mech3d
resetPaintScheme now gates on textureOrKtxSidecarExists so .ktx2-only works.

Per mech GLB, for every non-blip albedo image (base + skin variants):
  GLB-embedded PNG  --ktx create --encode uastc-->  UASTC  --transcode bc7-->  .ktx2
Output name = derived runtime tga stem + ".ktx2" (the sidecar name txmmgr derives by
stripping .tga and appending .ktx2). Deployed to data/tgl/ AND data/tgl/128/ (the
KTX_PRIMARY sidecar lookup does NOT strip the size subdir, and resetPaintScheme builds
data/tgl/<ObjectTextureSize>/<name>).

Full resolution preserved (2048^2, single mip level). Concern is format homogeneity,
not space. .tga albedos are removed after a successful cook (--keep-tga to retain).

Usage:
  py -3 cook_ktx2_textures.py --manifest MC2Conversions/conversions_manifest.json \
      --conversions MC2Conversions --deploy <release>/data/tgl \
      [--ktx A:/Games/mc2-tools/ktx/ktx.exe] [--only marauder,warhammer] [--keep-tga]
"""
import argparse, glob, io, json, os, subprocess, sys
from pygltflib import GLTF2

DEFAULT_KTX = r"A:/Games/mc2-tools/ktx/ktx.exe"


def img_png_bytes(g, im):
    bv = g.bufferViews[im.bufferView]
    blob = g.binary_blob()
    off = bv.byteOffset or 0
    return blob[off:off + bv.byteLength]


def cook_one_png(ktx, png_path, out_ktx2, tmp_uastc):
    r1 = subprocess.run([ktx, "create", "--encode", "uastc",
                         "--format", "R8G8B8A8_UNORM", "--assign-tf", "linear",
                         "--levels", "1", png_path, tmp_uastc],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r1.returncode != 0:
        print(f"   ktx create FAIL: {r1.stdout.strip()[:160]}"); return False
    r2 = subprocess.run([ktx, "transcode", "--target", "bc7", tmp_uastc, out_ktx2],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r2.returncode != 0:
        print(f"   ktx transcode FAIL: {r2.stdout.strip()[:160]}"); return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--conversions", required=True)
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--ktx", default=DEFAULT_KTX)
    ap.add_argument("--only", default="")
    ap.add_argument("--keep-tga", action="store_true")
    args = ap.parse_args()
    if not os.path.isfile(args.ktx):
        sys.exit(f"FATAL: ktx.exe not found: {args.ktx}")
    only = {s.strip().lower() for s in args.only.split(",") if s.strip()}
    with open(args.manifest, encoding="utf-8") as f:
        mechs = [r["btname"] for r in json.load(f)]

    tmp = os.path.join(args.conversions, "_ktxtmp")
    os.makedirs(tmp, exist_ok=True)
    size_dir = os.path.join(args.deploy, "128"); os.makedirs(size_dir, exist_ok=True)
    cooked = 0
    for bt in mechs:
        if only and bt.lower() not in only:
            continue
        glb = os.path.join(args.conversions, f"{bt}.glb")
        if not os.path.isfile(glb):
            continue
        g = GLTF2().load(glb)
        for im in (g.images or []):
            if not im.name:
                continue
            n = im.name.lower()
            # Cook albedo + AO (+ normal). All BC7 (the txmmgr KTX_PRIMARY
            # decoder accepts only BC7 vkFormat 145/146), homogeneous. blip atlas skipped.
            if "blip" in n or not any(k in n for k in ("alb", "amb", "nrm")):
                continue
            stem = os.path.splitext(im.name)[0].lower()        # chrtxrmech_<...>-alb
            png = os.path.join(tmp, stem + ".png")
            open(png, "wb").write(img_png_bytes(g, im))
            ktx2 = os.path.join(tmp, stem + ".ktx2")
            if not cook_one_png(args.ktx, png, ktx2, os.path.join(tmp, stem + ".uastc.ktx2")):
                continue
            import shutil
            for d in (args.deploy, size_dir):
                shutil.copy2(ktx2, os.path.join(d, stem + ".ktx2"))
                if not args.keep_tga:
                    old_tga = os.path.join(d, stem + ".tga")
                    if os.path.isfile(old_tga):
                        os.remove(old_tga)
            cooked += 1
            print(f"[ktx2] {bt}: {stem}.ktx2 (full-res BC7)" + ("" if args.keep_tga else " (+removed .tga)"))
    print(f"\n[ktx2] done: {cooked} albedo(s) cooked to BC7 .ktx2")


if __name__ == "__main__":
    main()

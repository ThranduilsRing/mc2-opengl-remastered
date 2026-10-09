#!/usr/bin/env python3
"""BT2018-MECH-MATERIAL-AO-1 / NORMALS-1: cook the AO + normal maps declared by packages.

Package-driven (honors the contract): for each deployed <chassis>.package.json that
declares materials.ao and/or materials.normal, cook its dump source PNG -> BC7 .ktx2
under the package-declared name (materials.<map>.ktx2), deploy to data/tgl/ AND
data/tgl/128/. AO and normal use the IDENTICAL cook command (BC7-UNORM LINEAR,
vkFormat 145). Normal stores full XYZ in RGB; missing normal is a no-op (same as AO).

AO logical format is linear single-channel; the cooked container is BC7 (the txmmgr
KTX_PRIMARY decoder accepts only BC7 145/146 — see the package "container":"bc7-compat").
The PNG is fed as-is (grayscale-in-RGB); only the R channel is read by the shader.

Usage:
  py -3 cook_ao_from_package.py --deploy <release>/data/tgl \
      --dump A:/Games/mc2-opengl/BattleTech_2018_Dump [--ktx A:/Games/mc2-tools/ktx/ktx.exe]
"""
import argparse, glob, json, os, shutil, subprocess, sys

import numpy as np
from PIL import Image

DEFAULT_KTX = r"A:/Games/mc2-tools/ktx/ktx.exe"

# Unity->OpenGL green-channel toggle. The BT2018 -nrm PNGs are Unity DXT5nm-packed
# (R=255 filler, G=normal Y, A=normal X, B=filler). Unity uses DirectX Y-down
# tangent space; MC2's shader expects OpenGL Y-up. Flipping G converts between them.
# THE ONE KNOB: flip this if bump/relief lighting looks inverted in-game.
FLIP_GREEN = True


def preprocess_unity_normal(src, tmp_png):
    """Unpack a Unity DXT5nm normal PNG into a clean OpenGL tangent-space RGB normal.
    R=255 filler / B=filler are discarded; X comes from ALPHA, Y from GREEN; Z is
    reconstructed outward (+Z). Output: full XYZ encoded 0..1 in RGB. Returns mean RGB."""
    a = np.asarray(Image.open(src).convert("RGBA")).astype(np.float32) / 255.0
    X = a[..., 3] * 2 - 1            # X from ALPHA
    Y = a[..., 1] * 2 - 1            # Y from GREEN
    if FLIP_GREEN:
        Y = -Y                       # Unity (Y-down) -> OpenGL (Y-up)
    Z = np.sqrt(np.clip(1 - X * X - Y * Y, 0, 1))   # reconstruct, always +Z (outward)
    out = np.stack([(X * 0.5 + 0.5), (Y * 0.5 + 0.5), (Z * 0.5 + 0.5)], -1)
    out = np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(out, "RGB").save(tmp_png)
    return [round(float(out[..., i].mean()), 1) for i in range(3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--dump", default="A:/Games/mc2-opengl/BattleTech_2018_Dump")
    ap.add_argument("--ktx", default=DEFAULT_KTX)
    args = ap.parse_args()
    if not os.path.isfile(args.ktx):
        sys.exit(f"FATAL: ktx.exe not found: {args.ktx}")
    tmp = os.path.join(args.deploy, "_aoktxtmp"); os.makedirs(tmp, exist_ok=True)
    size_dir = os.path.join(args.deploy, "128"); os.makedirs(size_dir, exist_ok=True)

    def cook_map(tag, mat, name):
        """Cook one dump-sourced material map (AO or normal) -> BC7 .ktx2, deploy
        to data/tgl/ AND data/tgl/128/. Returns 'cooked'/'skipped'/'none'. Both maps
        use the IDENTICAL command (BC7-UNORM LINEAR, vkFormat 145); normal stores full
        XYZ in RGB (no 2-channel — shader samples xyz directly)."""
        if not mat:
            return "none"
        src_ref = mat.get("source", "")
        if not src_ref.startswith("dump:"):
            print(f"[{tag}] {name}: source not a dump ref ({src_ref!r}) — SKIP"); return "skipped"
        src = os.path.join(args.dump, src_ref[len("dump:"):])
        if not os.path.isfile(src):
            print(f"[{tag}] {name}: dump source missing ({src}) — SKIP"); return "skipped"
        out_name = mat["ktx2"]                     # chrtxrmech_<chassis>-base-<amb|nrm>.ktx2
        # NORMAL maps only: BT2018 -nrm PNGs are Unity DXT5nm-packed (R=255 filler,
        # G=Y, A=X, B=filler). Cooking the raw PNG stores R=255 as "X" -> garbage.
        # Preprocess into a clean OpenGL XYZ-in-RGB normal, then feed THAT to ktx.
        # AO (and any other tag) is fed as-is, byte-for-byte unchanged.
        if tag == "nrm":
            pre = os.path.join(tmp, out_name + ".pre.png")
            mean = preprocess_unity_normal(src, pre)
            print(f"[{tag}] {name}: preprocessed normal mean RGB={mean}")
            src = pre
        uastc = os.path.join(tmp, out_name + ".uastc.ktx2")
        bc7 = os.path.join(tmp, out_name)
        r1 = subprocess.run([args.ktx, "create", "--encode", "uastc",
                             "--format", "R8G8B8A8_UNORM", "--assign-tf", "linear",
                             "--levels", "1", src, uastc],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if r1.returncode != 0:
            print(f"[{tag}] {name}: ktx create FAIL {r1.stdout.strip()[:140]}"); return "skipped"
        r2 = subprocess.run([args.ktx, "transcode", "--target", "bc7", uastc, bc7],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if r2.returncode != 0:
            print(f"[{tag}] {name}: ktx transcode FAIL {r2.stdout.strip()[:140]}"); return "skipped"
        for dd in (args.deploy, size_dir):
            shutil.copy2(bc7, os.path.join(dd, out_name))
        print(f"[{tag}] {name}: {out_name} (BC7, from {os.path.basename(src)})")
        return "cooked"

    ao_cooked = ao_skipped = nrm_cooked = nrm_skipped = nrm_none = 0
    for p in sorted(glob.glob(os.path.join(args.deploy, "*.package.json"))):
        name = os.path.basename(p)[:-len(".package.json")]
        d = json.load(open(p, encoding="utf-8"))
        mats = d.get("materials", {})
        r = cook_map("ao", mats.get("ao"), name)
        if r == "cooked": ao_cooked += 1
        elif r == "skipped": ao_skipped += 1
        r = cook_map("nrm", mats.get("normal"), name)
        if r == "cooked": nrm_cooked += 1
        elif r == "skipped": nrm_skipped += 1
        elif r == "none": nrm_none += 1
    print(f"\n[ao]  done: cooked={ao_cooked} skipped={ao_skipped}")
    print(f"[nrm] done: cooked={nrm_cooked} skipped={nrm_skipped} no-normal={nrm_none}")


if __name__ == "__main__":
    main()

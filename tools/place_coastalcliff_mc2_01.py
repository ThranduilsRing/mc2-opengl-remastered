#!/usr/bin/env python3
"""place_coastalcliff_mc2_01.py -- deploy-side swap of a REAL photogrammetry
nordic coastal cliff scan into the CLIFF_WALL mesh-decal (replacing the
procedural proof plane from gen_cliff_wall_glb.py).

The scan (large_nordic_coastal_cliff_ulujfanga_raw.glb, ~2.09M tris) was
imported, hard-decimated to ~15k tris, conditioned in Blender (single _PAB_
SHAPE node, upright with height->+Y / face->+/-Z, base sunk ~20u below origin,
outward normals, baseColor image renamed "coastal_cliff_01"), and exported as a
GLB. This script drops that GLB and its baseColor TGA into the deploy tree.

Reuses the proven MarbleCliff decoration mechanism unchanged:
  * deploy the conditioned GLB as data/tgl/CliffWallGLB.glb (keeps the stem the
    already-repointed marblecliff.ini Source="CliffWallGLB" + mc2_01 placement
    and ImGui panel reference).
  * extract the embedded "coastal_cliff_01" baseColor JPEG -> uncompressed 24bpp
    data/tgl/128/coastal_cliff_01.tga (static-prop texture binding is
    disk-.tga-name-based; assimp_importer.cpp DeriveMC2TextureName maps the
    image stem -> "coastal_cliff_01.tga"; a missing file draws BLACK).
  * inject_object (objType 1188 = MarbleCliff) at the cliff_side_on site --
    idempotent (place_marblecliff rebuilds packet 1 from its pristine backup).

Run AFTER deploy_payload.py so the deployed marblecliff.ini/pak exist.

Usage:
  py -3 tools/place_coastalcliff_mc2_01.py \
      --deploy-root "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0" \
      --glb ".../scratchpad/CoastalCliffGLB.glb" \
      [--basecolor-name coastal_cliff_01]
"""
from __future__ import annotations

import argparse
import io
import json
import struct
import sys
from pathlib import Path


def deploy_basecolor_tga(glb_path: Path, tgl_dir: Path, img_name: str) -> None:
    """Extract the GLB's embedded baseColor JPEG (image datablock == img_name)
    and write it as an uncompressed 24bpp BGR top-left TGA to
    data/tgl/128/<img_name>.tga -- the name MC2 derives from the baseColor stem.
    """
    data = glb_path.read_bytes()
    off = 12
    jlen, _ = struct.unpack("<II", data[off:off + 8]); off += 8
    js = json.loads(data[off:off + jlen].decode("utf-8")); off += jlen
    blen, _ = struct.unpack("<II", data[off:off + 8]); off += 8
    binblob = data[off:off + blen]
    bvs = js["bufferViews"]

    # Prefer the material's actual baseColor texture image; fall back to name match.
    jpg = None
    imgs = js.get("images", [])
    tex = js.get("textures", [])
    mats = js.get("materials", [])
    bc_img_idx = None
    if mats:
        bc = mats[0].get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if bc is not None:
            bc_img_idx = tex[bc["index"]]["source"]
    cand_idx = bc_img_idx
    if cand_idx is None:
        for i, im in enumerate(imgs):
            if im.get("name") == img_name:
                cand_idx = i
                break
    if cand_idx is None:
        raise SystemExit(f"GLB has no baseColor / '{img_name}' image")
    im = imgs[cand_idx]
    if "bufferView" not in im:
        raise SystemExit("baseColor image is not embedded in the GLB binary chunk")
    bv = bvs[im["bufferView"]]
    o = bv.get("byteOffset", 0)
    jpg = binblob[o:o + bv["byteLength"]]

    from PIL import Image
    pim = Image.open(io.BytesIO(jpg)).convert("RGB").resize((256, 256), Image.LANCZOS)
    w, h = pim.size
    px = pim.tobytes()  # RGB
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 24, 0x20)
    body = bytearray()
    for i in range(0, len(px), 3):
        body += bytes((px[i + 2], px[i + 1], px[i]))

    size_dir = tgl_dir / "128"  # ObjectTextureSize default (bdactor.cpp:665)
    size_dir.mkdir(parents=True, exist_ok=True)
    dst = size_dir / f"{img_name}.tga"
    dst.write_bytes(hdr + bytes(body))
    print(f"[tga] baseColor '{img_name}' -> {dst} ({dst.stat().st_size} bytes, {w}x{h})")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--deploy-root", required=True)
    ap.add_argument("--glb", required=True, help="conditioned CoastalCliff GLB")
    ap.add_argument("--basecolor-name", default="coastal_cliff_01")
    args = ap.parse_args(argv)

    root = Path(args.deploy_root)
    tgl = root / "data" / "tgl"
    ini = tgl / "marblecliff.ini"
    pak = root / "data" / "missions" / "mc2_01.pak"
    src_glb = Path(args.glb)
    for p in (tgl, ini, pak):
        if not p.exists():
            raise SystemExit(f"missing prerequisite: {p} (run deploy_payload first)")
    if not src_glb.exists():
        raise SystemExit(f"missing conditioned GLB: {src_glb}")

    # Keep the CliffWallGLB stem so the already-repointed marblecliff.ini
    # Source="CliffWallGLB" + placement + ImGui panel work unchanged.
    dst_glb = tgl / "CliffWallGLB.glb"
    dst_glb.write_bytes(src_glb.read_bytes())
    print(f"[glb] deployed -> {dst_glb} ({dst_glb.stat().st_size} bytes)")

    deploy_basecolor_tga(dst_glb, tgl, args.basecolor_name)

    # Ensure the ini Source points at CliffWallGLB (idempotent).
    txt = ini.read_text()
    if "CliffWallGLB" not in txt:
        out = []
        for line in txt.splitlines():
            if line.strip().lower().startswith("st source"):
                out.append('st Source = "CliffWallGLB"')
            else:
                out.append(line)
        ini.write_text("\n".join(out) + "\n")
        print(f"[ini] Source -> \"CliffWallGLB\" ({ini})")
    else:
        print(f"[ini] Source already -> CliffWallGLB (unchanged)")

    # Reuse place_marblecliff's idempotent object injection (objType 1188).
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from place_marblecliff_mc2_01 import inject_object
    inject_object(pak)

    print("done. capture with scripts/run_visual_capture.py --mission mc2_01 "
          "(gate MC2_TERRAIN_DECAL=1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

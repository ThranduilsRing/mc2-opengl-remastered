#!/usr/bin/env python3
"""place_cliffwall_mc2_01.py -- deploy-side placement of the CLIFF_WALL mesh
decal (terrain mesh-decal system, Slice 0A visual proof).

Reuses the proven MarbleCliff decoration mechanism (object2.pak packet 1188 ->
marblecliff.ini -> a GLB in data/tgl) but repoints it at the GENERATED cliff-wall
mesh (tools/gen_cliff_wall_glb.py output) instead of the photogrammetry rock
blob. The engine still registers the appearance under the name "MarbleCliff";
the gated C++ CLIFF_WALL face-frame (bdactor.cpp registerStatic, gate
MC2_TERRAIN_DECAL) stands it up as a wall.

Deploy-side edits (no C++, all idempotent with .bak backups):
  1. Copy the generated CliffWallGLB.glb into <deploy>/data/tgl/.
  2. Repoint <deploy>/data/tgl/marblecliff.ini  [Import] Source -> "CliffWallGLB".
  3. Inject one terrain-object record (objType 1188 = MarbleCliff) into
     mc2_01.pak packet 1 at the cliff_side_on site (via place_marblecliff's
     inject_object; NO GLB bake -- our GLB is pre-scaled + _PAB-named).

Run AFTER deploy_payload.py so the deployed marblecliff.ini/pak exist.

Usage:
  python place_cliffwall_mc2_01.py --deploy-root "<...>/mc2-win64-v0.5.0" \
      --glb C:/Users/Joe/AppData/Local/Temp/CliffWallGLB.glb
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def deploy_basecolor_tga(glb_path: Path, tgl_dir: Path) -> None:
    """Extract the GLB's embedded baseColor JPEG and write it as an uncompressed
    24bpp TGA to data/tgl/<ObjectTextureSize>/marble_cliff_01.tga.

    WHY: MC2 imports a GLB static prop by deriving a texture *name* from the
    baseColor image stem (assimp_importer.cpp DeriveMC2TextureName -> here
    "marble_cliff_01" -> "marble_cliff_01.tga"). The importer NEVER decodes the
    embedded JPEG; LoadOverrideRenderShapeTextures (bdactor.cpp) resolves that
    name to a GL handle ONLY by loading data/tgl/<ObjectTextureSize>/<name>.tga
    from disk -- a missing file leaves the slot at 0xffffffff and the wall draws
    BLACK/untextured. ObjectTextureSize defaults to 128 (bdactor.cpp:665), so
    the .tga must live in the tgl/128 subdir. This step makes the derived name
    resolve so the cliff wall renders as a lit marble rock face.
    """
    import struct
    import json

    data = glb_path.read_bytes()
    off = 12
    jlen, _ = struct.unpack("<II", data[off:off + 8]); off += 8
    js = json.loads(data[off:off + jlen].decode("utf-8")); off += jlen
    blen, _ = struct.unpack("<II", data[off:off + 8]); off += 8
    binblob = data[off:off + blen]
    bvs = js["bufferViews"]
    jpg = None
    for img in js.get("images", []):
        if img.get("name") == "marble_cliff_01" and "bufferView" in img:
            bv = bvs[img["bufferView"]]
            o = bv.get("byteOffset", 0)
            jpg = binblob[o:o + bv["byteLength"]]
            break
    if jpg is None:
        raise SystemExit("GLB has no embedded 'marble_cliff_01' baseColor image")

    from PIL import Image
    import io
    im = Image.open(io.BytesIO(jpg)).convert("RGB").resize((256, 256), Image.LANCZOS)
    w, h = im.size
    px = im.tobytes()  # RGB
    # Uncompressed truecolor TGA, 24bpp, BGR, top-left origin (desc bit5=0x20) --
    # matches the format of the stock data/tgl/128/*.tga object textures.
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 24, 0x20)
    body = bytearray()
    for i in range(0, len(px), 3):
        body += bytes((px[i + 2], px[i + 1], px[i]))

    size_dir = tgl_dir / "128"  # ObjectTextureSize default (bdactor.cpp:665)
    size_dir.mkdir(parents=True, exist_ok=True)
    dst = size_dir / "marble_cliff_01.tga"
    dst.write_bytes(hdr + bytes(body))
    print(f"[tga] baseColor -> {dst} ({dst.stat().st_size} bytes, {w}x{h})")


def repoint_ini(ini_path: Path, new_stem: str) -> None:
    bak = ini_path.with_suffix(ini_path.suffix + ".bak_cliffwall")
    if not bak.exists():
        bak.write_bytes(ini_path.read_bytes())
        print(f"[ini] backup -> {bak.name}")
    text = bak.read_text()
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.lower().startswith("st source"):
            out.append(f'st Source = "{new_stem}"')
        else:
            out.append(line)
    ini_path.write_text("\n".join(out) + "\n")
    print(f"[ini] Source -> \"{new_stem}\"  ({ini_path})")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--deploy-root", required=True)
    ap.add_argument("--glb", required=True, help="generated CliffWallGLB.glb")
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
        raise SystemExit(f"missing generated GLB: {src_glb}")

    dst_glb = tgl / "CliffWallGLB.glb"
    dst_glb.write_bytes(src_glb.read_bytes())
    print(f"[glb] deployed -> {dst_glb} ({dst_glb.stat().st_size} bytes)")

    # Static-prop texture binding is disk-.tga-name-based (embedded GLB image is
    # NOT decoded) -- deploy the derived marble_cliff_01.tga or the wall is black.
    deploy_basecolor_tga(dst_glb, tgl)

    repoint_ini(ini, "CliffWallGLB")

    # Reuse place_marblecliff's proven object injection (objType 1188 at the
    # cliff_side_on site). No GLB bake -- our GLB is already world-scaled + _PAB.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from place_marblecliff_mc2_01 import inject_object
    inject_object(pak)

    print("done. capture with scripts/run_visual_capture.py --mission mc2_01 "
          "(gate MC2_TERRAIN_DECAL=1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

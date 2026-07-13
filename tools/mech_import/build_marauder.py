"""
build_marauder.py — one-shot driver: BT2018 marauder dump -> MC2 'madcat' override.

Stage 1 (static, POC). Runs the Blender geometry pass, then PIL-converts the
albedo PNG(s) to the .tga name(s) the MC2 importer derives. Prints the deploy
recipe at the end (does NOT deploy — copy is a deliberate manual step).

    py -3 build_marauder.py [--weapons] [--scale 1.0] [--dump <BT2018_dump>]

Output under ./_out:
    _out/MadCat.glb            -> deploy to <game>/data/tgl/MadCat.glb
    _out/tex/marauder_body.tga -> deploy to <game>/data/tgl/<size>/marauder_body.tga
    _out/tex/marauder_wpn.tga  (only with --weapons)
"""
import argparse
import json
import struct
from pathlib import Path

from blender_runner import find_blender_exe, run_blender_script

HERE = Path(__file__).parent
DEF_DUMP = Path(r"A:/Games/mc2-opengl/BattleTech_2018_Dump")


def derive_mc2_tex_name(image_name: str) -> str:
    """Mirror assimp_importer.cpp DeriveMC2TextureName: strip dir/ext, lowercase,
    sanitize to [a-z0-9_-] (else '_'), append .tga. The engine looks for this
    exact filename in data/tgl/<size>/, so our TGA must match it byte-for-byte."""
    stem = Path(image_name.replace("\\", "/")).name
    if "." in stem:
        stem = stem[: stem.rfind(".")]
    out = []
    for c in stem.lower():
        out.append(c if (c.isascii() and (c.isalnum() or c in "_-")) else "_")
    return "".join(out) + ".tga"


def glb_material_images(glb_path: Path):
    """Return {material_name: embedded_image_name} from a GLB's baseColorTexture."""
    d = glb_path.read_bytes()
    _, _, length = struct.unpack("<III", d[:12])
    off, js = 12, None
    while off < length:
        clen, ctype = struct.unpack("<II", d[off:off + 8]); off += 8
        chunk = d[off:off + clen]; off += clen
        if ctype == 0x4E4F534A:
            js = json.loads(chunk)
    res = {}
    texs, imgs = js.get("textures", []), js.get("images", [])
    for m in js.get("materials", []):
        bct = m.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if bct is None:
            continue
        src = texs[bct["index"]].get("source")
        if src is None:
            continue
        res[m.get("name", "")] = imgs[src].get("name", "")
    return res


def png_to_tga(src: Path, dst: Path, max_dim: int = 0):
    from PIL import Image
    img = Image.open(src).convert("RGBA")
    if max_dim and max(img.size) > max_dim:
        img.thumbnail((max_dim, max_dim), Image.LANCZOS)
    dst.parent.mkdir(parents=True, exist_ok=True)
    # MC2 TGAs are conventional; PIL writes uncompressed 32-bit TGA.
    img.save(dst, format="TGA")
    print(f"  tex {dst.name} <- {src.name} {img.size}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weapons", action="store_true", help="include weapon hardpoints (2nd material)")
    ap.add_argument("--scale", default="1.0")
    ap.add_argument("--max-dim", type=int, default=0, help="downscale albedo to this max edge (0=keep 2048)")
    ap.add_argument("--dump", default=str(DEF_DUMP))
    ap.add_argument("--out", default=str(HERE / "_out"))
    args = ap.parse_args()

    dump = Path(args.dump)
    out = Path(args.out)
    out_glb = out / "MadCat.glb"
    out_tex = out / "tex"

    base_alb = dump / "Animator/chrPrfMech_marauderBase-001/chrTxrMech_marauder-Base-alb.png"
    wpn_alb = dump / "Texture2D/chrTxrMech_marauder-Weapons-alb.png"
    mesh_dir = dump / "MadCat/Mesh"
    for p in (base_alb, mesh_dir):
        if not p.exists():
            raise SystemExit(f"missing input: {p}")

    print("[build_marauder] geometry pass (Blender)...")
    bargs = {
        "mesh_dir": str(mesh_dir),
        "tex_base": str(base_alb),
        "out_glb": str(out_glb),
        "out_tex_dir": str(out_tex),
        "weapons": "1" if args.weapons else "0",
        "scale": args.scale,
    }
    if args.weapons:
        bargs["tex_wpn"] = str(wpn_alb)
    log = run_blender_script(find_blender_exe(), str(HERE / "mech_prep_marauder.py"), bargs)
    for l in log.splitlines():
        if "[MARAUDER]" in l:
            print("  " + l.split("[MARAUDER]", 1)[1].strip())

    print("[build_marauder] texture pass (PIL, names derived from GLB)...")
    # Source PNG per material (we named the materials in mech_prep_marauder.py).
    src_for_mat = {"marauder_body": base_alb, "marauder_wpn": wpn_alb}
    mat_imgs = glb_material_images(out_glb)
    if not mat_imgs:
        raise SystemExit("GLB has no baseColorTexture materials — texture pass cannot derive names")
    for mat_name, img_name in mat_imgs.items():
        src = src_for_mat.get(mat_name)
        if src is None or not src.exists():
            print(f"  WARN: no source PNG for material {mat_name!r}; skipping")
            continue
        tga_name = derive_mc2_tex_name(img_name)  # exactly what the engine loads
        png_to_tga(src, out_tex / tga_name, args.max_dim)

    print("\n[build_marauder] DONE. Deploy recipe (manual):")
    print(f"  copy {out_glb}  ->  <game>/data/tgl/MadCat.glb")
    print(f"  copy {out_tex}/*.tga  ->  <game>/data/tgl/  AND  <game>/data/tgl/<ObjectTextureSize>/")
    print("  ensure madcat.ini has [Import] Source=\"MadCat\"  (REQUIRED opt-in; engine probes .glb/.fbx, falls back .ase)")
    print("  no env var needed (build must have ENABLE_ASSIMP_IMPORTER) ; check in mc2_24 (no intro cinematic)")
    print("  debug: MC2_ASSIMP_TRACE=1 traces probes ; MC2_GLTF_AXIS/_YOFF/_GROUND tune orientation/lift")


if __name__ == "__main__":
    main()

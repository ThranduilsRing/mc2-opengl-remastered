#!/usr/bin/env python3
"""tools/asset_cook/trackg_cook.py — Track G offline static-prop cook driver.

Subcommands:
  stage   source.glb -> cooked.glb (convention-frozen) + staged.json geometry fragment

`stage` computes geometry (bounds/pivot/counts) in MC2 runtime-importer space by
REPLICATING mclib/assimp_importer.cpp's default-env transform — NOT the workbench
GlbMeshLoader (which uses a different convention; see the R0 convention note). The
cooked glb for an already-default-correct source (e.g. bigbox) is a passthrough copy;
meshopt optimization is a later add. Material *discovery* here is names + alphaClass
only; KTX2 texture cook is G2.

Frozen runtime convention (assimp_importer.cpp, default env):
  axisMap(0): X=-x, Y=-y, Z=z      (assimp_importer.cpp:60-69)
  toMC2Pos:   (X, Y + YOFF(0), Z)  (:71)
  auto-ground GROUND=2: dy = -minBox.y, translate Y so base sits at 0 (:544)

Does NOT cook textures, build GL state, write models.json, or touch the engine.

Usage:
  py -3 tools/asset_cook/trackg_cook.py stage <source.glb> <out_dir> \
       --id bigbox --class staticprop --appearance hangar
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

KTX_CLI = r"A:/Games/mc2-tools/ktx/ktx.exe"
DEFAULT_TIERS = (128, 256, 512, 1024)

# ---- glTF accessor decode -------------------------------------------------

_COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
         5125: ("I", 4), 5126: ("f", 4)}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path: Path):
    d = path.read_bytes()
    if d[:4] != b"glTF":
        raise ValueError(f"{path}: not a GLB (magic {d[:4]!r})")
    off, js, binblob = 12, None, b""
    while off < len(d):
        clen, ctype = struct.unpack_from("<I4s", d, off)
        off += 8
        chunk = d[off:off + clen]
        off += clen
        if ctype == b"JSON":
            js = json.loads(chunk)
        elif ctype == b"BIN\x00":
            binblob = chunk
    if js is None:
        raise ValueError(f"{path}: no JSON chunk")
    return js, binblob


def accessor_array(js, binblob, idx) -> np.ndarray:
    acc = js["accessors"][idx]
    bv = js["bufferViews"][acc["bufferView"]]
    comp, csize = _COMP[acc["componentType"]]
    n = _NCOMP[acc["type"]]
    count = acc["count"]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 0) or (csize * n)
    out = np.empty((count, n), dtype=np.float64)
    for i in range(count):
        vals = struct.unpack_from("<" + comp * n, binblob, base + i * stride)
        out[i] = vals
    return out


def node_matrix(node) -> np.ndarray:
    if "matrix" in node:  # column-major 16
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
    m = np.eye(4)
    if "scale" in node:
        m = np.diag([*node["scale"], 1.0]) @ m
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        r = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
            [0, 0, 0, 1]])
        m = r @ m
    if "translation" in node:
        t = np.eye(4); t[:3, 3] = node["translation"]
        m = t @ m
    return m


# ---- runtime importer transform replica -----------------------------------

def axis_map0(p: np.ndarray) -> np.ndarray:
    """assimp_importer.cpp axisMap(0): X=-x, Y=-y, Z=z."""
    out = p.copy()
    out[:, 0] = -p[:, 0]
    out[:, 1] = -p[:, 1]
    out[:, 2] = p[:, 2]
    return out


def stage(args) -> int:
    src = Path(args.source)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    js, binblob = read_glb(src)

    # gather world-space positions per primitive (compose node transforms)
    prim_positions: list[np.ndarray] = []
    materials_discovered = []
    nonidentity = False
    for node in js.get("nodes", []):
        if "mesh" not in node:
            continue
        M = node_matrix(node)
        if not np.allclose(M, np.eye(4)):
            nonidentity = True
        mesh = js["meshes"][node["mesh"]]
        for pr in mesh["primitives"]:
            if pr.get("mode", 4) != 4:
                continue  # non-triangle prim
            pos = accessor_array(js, binblob, pr["attributes"]["POSITION"])
            ph = np.hstack([pos, np.ones((len(pos), 1))])
            world = (M @ ph.T).T[:, :3]
            prim_positions.append(world)
            # tri count
            if "indices" in pr:
                tris = js["accessors"][pr["indices"]]["count"] // 3
            else:
                tris = len(pos) // 3
            # material discovery (names + alphaClass only; ktx2 is G2)
            mat_idx = pr.get("material")
            mname, alpha = "NULLTXM", 0
            if mat_idx is not None:
                mat = js["materials"][mat_idx]
                raw = mat.get("name", f"mat{mat_idx}")
                mname = "".join(c.lower() if c.isalnum() else "_" for c in raw)
                if mat.get("alphaMode") in ("MASK", "BLEND"):
                    alpha = 1
            materials_discovered.append(
                {"textureName": ("a_" + mname if alpha and not mname.startswith("a_") else mname),
                 "alphaClass": alpha, "verts": len(pos), "tris": tris})

    if not prim_positions:
        print(f"FAIL {src}: no triangulated geometry")
        return 1

    allpos = np.vstack(prim_positions)
    mc2 = axis_map0(allpos)
    mc2[:, 1] += args.yoff  # YOFF default 0

    pre_min = mc2.min(axis=0)
    # auto-ground GROUND=2: dy = -minBox.y
    dy = -pre_min[1] if args.ground == 2 else (-mc2.max(axis=0)[1] if args.ground == 1 else 0.0)
    mc2[:, 1] += dy

    bmin = mc2.min(axis=0)
    bmax = mc2.max(axis=0)
    center = (bmin + bmax) / 2.0
    radius = float(np.max(np.linalg.norm(mc2 - center, axis=1)))  # bounding-sphere about bbox center

    verts = sum(m["verts"] for m in materials_discovered)
    tris = sum(m["tris"] for m in materials_discovered)
    submeshes = len(materials_discovered)

    geometry = {
        "source": args.source_rel or src.name,
        "cooked": src.name,
        "convention": {"axis": 0, "vflip": True, "importer": "assimp_importer.v1"},
        "scale": 1.0,
        "bounds": {"min": [round(float(v), 4) for v in bmin],
                   "max": [round(float(v), 4) for v in bmax],
                   "radius": round(radius, 4)},
        "pivot": [0.0, 0.0, 0.0],
        "counts": {"verts": verts, "tris": tris, "submeshes": submeshes},
        "lods": [],
    }
    staged = {
        "asset": {"id": args.id, "class": args._class,
                  "appearanceName": args.appearance,
                  "replaces": f"{args._class}:{args.appearance}"},
        "geometry": geometry,
        "materials_discovered": [{"slot": i, **{k: m[k] for k in ("textureName", "alphaClass")}}
                                 for i, m in enumerate(materials_discovered)],
        "warnings": (["non-identity node transform composed"] if nonidentity else []),
    }

    # cook glb = passthrough (already default-env-correct); meshopt is a later add
    cooked = out_dir / src.name
    if cooked.resolve() != src.resolve():
        shutil.copyfile(src, cooked)
    (out_dir / "staged.json").write_text(json.dumps(staged, indent=2), encoding="utf-8")
    print(f"STAGED {src.name} -> {out_dir}/  bounds={geometry['bounds']['min']}..{geometry['bounds']['max']} "
          f"r={geometry['bounds']['radius']} verts={verts} tris={tris} submeshes={submeshes}")
    return 0


# ---- G2: KTX2 material cook (reuses the cook_tgl_tiers.py ktx.exe recipe) ---

def _resized(img, cap: int):
    from PIL import Image
    w, h = img.size
    if max(w, h) <= cap:
        return img  # never upscale
    if w >= h:
        nw, nh = cap, max(1, round(h * cap / w))
    else:
        nw, nh = max(1, round(w * cap / h)), cap
    return img.resize((nw, nh), Image.LANCZOS)


def _ktx_cook(img, dst: Path, srgb: bool):
    """uastc encode -> bc7 transcode (stored BC7; the loader rejects supercompressed).
    Mirrors tools/mc2texcook/cook_tgl_tiers.py::_ktx_cook."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    fmt = "R8G8B8A8_SRGB" if srgb else "R8G8B8A8_UNORM"
    tf = "srgb" if srgb else "linear"
    with tempfile.TemporaryDirectory() as td:
        png = Path(td) / "s.png"
        uastc = Path(td) / "u.ktx2"
        img.convert("RGBA").save(png, "PNG")
        for step in (
            [KTX_CLI, "create", "--encode", "uastc", "--format", fmt,
             "--assign-tf", tf, "--generate-mipmap", str(png), str(uastc)],
            [KTX_CLI, "transcode", "--target", "bc7", str(uastc), str(dst)],
        ):
            r = subprocess.run(step, capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(f"ktx {step[1]} rc={r.returncode}: {(r.stderr or r.stdout).strip()[:200]}")


def ktx2_format(path: Path) -> int:
    """Return vkFormat (145/146 = BC7 UNORM/SRGB) or -1. Asserts no supercompression."""
    with open(path, "rb") as f:
        if f.read(12) != b"\xabKTX 20\xbb\r\n\x1a\n":
            return -1
        vk, _ts, _w, _h, _d, _l, _fc, _lv, sc = struct.unpack("<9I", f.read(36))
    return vk if sc == 0 else -1


def textures(args) -> int:
    from PIL import Image
    if not Path(KTX_CLI).is_file():
        print(f"FAIL: ktx CLI missing: {KTX_CLI}")
        return 1
    staged = json.loads(Path(args.staged).read_text(encoding="utf-8"))
    tex_dir = Path(args.texture_dir)
    out_root = Path(args.out_root)
    tiers = [int(t) for t in args.tiers.split(",") if t.strip()]

    materials, warnings = [], []
    for m in staged.get("materials_discovered", []):
        slot = m["slot"]
        tname = m["textureName"]            # carries a_ prefix iff alpha
        alpha = int(m.get("alphaClass", 0))
        base = tname[2:] if tname.startswith("a_") else tname
        # find source image (base.png / base.tga)
        srcimg = next((p for ext in (".png", ".tga", ".PNG", ".TGA")
                       if (p := tex_dir / f"{base}{ext}").exists()), None)
        if srcimg is None:
            warnings.append(f"slot {slot}: no source texture for {base!r} -> NULLTXM (untextured)")
            continue
        img = Image.open(srcimg)
        if alpha and img.mode != "RGBA":
            img = img.convert("RGBA")
        tiermap = {}
        for cap in tiers:
            dstdir = out_root / "data" / "tgl" / str(cap)
            dst = dstdir / f"{tname}.ktx2"
            _ktx_cook(_resized(img, cap), dst, srgb=True)  # albedo = sRGB
            vk = ktx2_format(dst)
            if vk not in (145, 146):
                print(f"FAIL: {dst} not stored BC7 (vkFormat={vk})")
                return 1
            # MODEL-OVERRIDE render path (bdactor.cpp LoadOverrideRenderShapeTextures)
            # loads textures by '<name>.tga' via mcTextureManager and CANNOT decode
            # BC7/.ktx2 — it gates on fileExists(<name>.tga). So emit an uncompressed
            # TGA next to the ktx2 (this is the file the override loader actually
            # reads; the lush/oak override path used .tga for the same reason).
            tga = dstdir / f"{tname}.tga"
            timg = _resized(img, cap)
            if alpha and timg.mode != "RGBA":
                timg = timg.convert("RGBA")
            timg.save(tga)
            tiermap[str(cap)] = f"data/tgl/{cap}/{tname}.ktx2"
        materials.append({
            "slot": slot, "textureName": tname, "alphaClass": alpha,
            "albedo_ktx2": tiermap,
            "normal_ktx2": None, "metallic_roughness_ktx2": None, "emissive_ktx2": None,
            "base_color_factor": 1.0, "metallic_factor": 0.0, "roughness_factor": 1.0,
            "flags": {"alpha_test": bool(alpha), "double_sided": False, "window": False},
        })
    out = {"materials": materials, "warnings": warnings}
    Path(args.out_json).write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(f"TEXTURES {len(materials)} material(s) cooked to BC7 tiers {tiers} -> {args.out_json}"
          + (f"  ({len(warnings)} warn)" if warnings else ""))
    return 0


# ---- G3b: assemble full manifest + project models.generated.json ----------

def _is_safe_source(s: str) -> bool:
    """Mirror model_override_registry.cpp isSafeSource(): relative, no drive/.., glb/gltf."""
    if not s or s[0] in "/\\" or (len(s) > 1 and s[1] == ":"):
        return False
    if ".." in s.replace("\\", "/").split("/"):
        return False
    return s.lower().endswith((".glb", ".gltf"))


def registry_resolves(entry: dict) -> tuple[bool, str]:
    """Python mirror of ModelOverrideRegistry::loadFromFile per-entry accept rules.
    The authoritative gate is the C++ ExportBundle round-trip; this catches drift offline."""
    if entry.get("type") != "model":
        return False, "type != model"
    if entry.get("renderOnly") is not True:
        return False, "renderOnly != true"
    if entry.get("fallback") != "stock":
        return False, "fallback != stock"
    if entry.get("scale") != 1.0:
        return False, "scale != 1.0"
    cls = str(entry.get("class", "")).lower()
    if cls not in ("staticprop", "tree"):
        return False, f"class {cls!r} not staticprop|tree"
    rep = entry.get("replaces", "")
    if ":" not in rep:
        return False, "replaces has no ':'"
    rcls, rname = rep.split(":", 1)
    if rcls.lower() != cls or not rname:
        return False, "replaces class/name mismatch"
    if not _is_safe_source(entry.get("source", "")):
        return False, f"unsafe source {entry.get('source')!r}"
    return True, "ok"


def assemble(args) -> int:
    import hashlib
    staged = json.loads(Path(args.staged).read_text(encoding="utf-8"))
    mats = json.loads(Path(args.materials).read_text(encoding="utf-8")).get("materials", [])
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    asset = staged["asset"]
    geom = staged["geometry"]
    lods = geom.get("lods", []) or []
    any_alpha = any(m.get("alphaClass", 0) == 1 for m in mats)

    textures = sorted({p for m in mats for p in (m.get("albedo_ktx2") or {}).values()})
    src_sha = ""
    cooked_glb = out_dir / geom["cooked"]
    if cooked_glb.exists():
        src_sha = hashlib.sha256(cooked_glb.read_bytes()).hexdigest()

    manifest = {
        "schema": "mc2-asset-manifest-v1",
        "cookVersion": 1,
        "asset": asset,
        "geometry": geom,
        "materials": mats,
        "capabilities": {
            "hasLegacyMesh": bool(args.has_legacy),
            "hasCookedGlb": True,
            "hasLodChain": len(lods) > 0,
            "hasMeshlets": False,
            "hasImpostor": False,
            "alphaTest": any_alpha,
            "castsShadow": bool(args.casts_shadow),
            "supportsObjectId": True,
        },
        "deps": {
            "stockFallback": asset["appearanceName"],
            "textures": textures,
            "sourceGlb": geom["source"],
        },
        "provenance": {
            "sourceSha256": src_sha,
            "cookTools": {"ktx": "ktx-software-v4.4.2", "driver": "trackg_cook.v1"},
            "cookedUtc": args.cooked_utc,
        },
    }
    if not mats:
        # schema requires >=1 material; an untextured prop still needs a slot.
        print("FAIL assemble: no cooked materials (schema requires materials[] >= 1). "
              "Cook at least one albedo (or add a placeholder) before assembling.")
        return 1

    # runtime projection (the subset the registry consumes)
    override_source = args.override_source or f"cooked/{asset['id']}/{geom['cooked']}"
    entry = {"type": "model", "class": asset["class"], "replaces": asset["replaces"],
             "source": override_source, "renderOnly": True, "scale": 1.0, "fallback": "stock"}
    if lods:
        entry["lods"] = [{"lod": l["lod"], "source": args_override_lod(args, asset, l),
                          "distance": l.get("distance", 0.0)} for l in lods]
    ok, why = registry_resolves(entry)
    if not ok:
        print(f"FAIL assemble: projected models.generated.json would NOT resolve in registry: {why}")
        return 1

    # SAFETY (Patch 4): never write a file literally named models.json (the central manifest).
    mpath = out_dir / "manifest.json"
    gpath = out_dir / "models.generated.json"
    for p in (mpath, gpath):
        if p.name == "models.json":
            print(f"FAIL assemble: refusing to write central manifest name {p}")
            return 1

    # validate the full manifest against the schema before writing
    mpath.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    rc = subprocess.run([sys.executable, str(Path(__file__).with_name("validate_asset_manifest.py")),
                         str(mpath)], capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if rc.returncode != 0:
        print("FAIL assemble: assembled manifest failed schema/coherence:\n" + rc.stdout + rc.stderr)
        return 1

    gpath.write_text(json.dumps({"overrides": [entry]}, indent=2), encoding="utf-8")
    print(f"ASSEMBLED {asset['replaces']} -> {mpath.name} (schema OK) + {gpath.name} "
          f"(registry-resolvable; source={override_source})")
    return 0


def args_override_lod(args, asset, l):
    return f"cooked/{asset['id']}/{l['cooked']}"


def main() -> int:
    ap = argparse.ArgumentParser(description="Track G offline static-prop cook driver.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage", help="source.glb -> cooked glb + staged.json geometry fragment")
    s.add_argument("source", type=str)
    s.add_argument("out_dir", type=str)
    s.add_argument("--id", required=True)
    s.add_argument("--class", dest="_class", required=True, choices=["staticprop", "tree"])
    s.add_argument("--appearance", required=True)
    s.add_argument("--source-rel", default=None, help="source path recorded in manifest (rel to deploy root)")
    s.add_argument("--ground", type=int, default=2, help="MC2_GLTF_GROUND (default 2)")
    s.add_argument("--yoff", type=float, default=0.0, help="MC2_GLTF_YOFF (default 0)")
    s.set_defaults(func=stage)

    t = sub.add_parser("textures", help="cook discovered materials' albedo -> BC7 KTX2 tiers + materials.json")
    t.add_argument("--staged", required=True, help="staged.json from `stage`")
    t.add_argument("--texture-dir", required=True, help="dir of source <textureName>.{png,tga}")
    t.add_argument("--out-root", required=True, help="deploy root; tiers land under <root>/data/tgl/<tier>/")
    t.add_argument("--out-json", required=True, help="materials.json fragment to write")
    t.add_argument("--tiers", default="128,256,512,1024")
    t.set_defaults(func=textures)

    a = sub.add_parser("assemble", help="staged.json + materials.json -> full manifest.json + models.generated.json")
    a.add_argument("--staged", required=True)
    a.add_argument("--materials", required=True)
    a.add_argument("--out-dir", required=True, help="bundle-local cooked/<id>/ dir")
    a.add_argument("--override-source", default=None, help="source path the registry loads (rel to data/model_overrides); default cooked/<id>/<glb>")
    a.add_argument("--casts-shadow", type=int, default=1)
    a.add_argument("--has-legacy", type=int, default=1)
    a.add_argument("--cooked-utc", default="", help="provenance stamp (default empty for determinism)")
    a.set_defaults(func=assemble)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

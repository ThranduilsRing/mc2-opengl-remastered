#!/usr/bin/env python3
"""Build one MW5 mech pack from its recipe: extract, hull GLB, cockpit, bake, cook, pack.

Every per-mech fact lives in recipes/<family>.json (object paths, clip names, hardpoint table, cockpit bone).
Game bytes stay in --work (outside git); the pack is written to <install>/mods/<mod id>/.

  py -3 tools/mw5_mechs/make_mech.py --recipe recipes/hunchback.json --work G:/mc2-tmp/x/hunchback \
      --install E:/mc2-installs/mw5-poc [--steps extract,glb,bake,pack] [--mod-id mw5_mechs]

Environment: MW5_INSTALL (game root), MW5_EXTRACT_DLL (Mw5Extract.dll), UMODEL (umodel_64.exe), KTX (ktx.exe),
MW5_MATERIAL_DEFAULTS (json of the shared painted-metal material's default parameters).
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))
import add_cockpit  # noqa: E402
import bake_extras  # noqa: E402
import bake_material  # noqa: E402
import build_glb  # noqa: E402
import build_pack  # noqa: E402
import cook_textures  # noqa: E402
import from_native  # noqa: E402
import prone_pose  # noqa: E402
from sockets import load_glb  # noqa: E402

IDLE = "A:/Games/mc2-ops/idle_run.py"
PAINT_KEYS = ("PaintColorPrimary", "PaintColorSecondary", "PaintColorTertiary", "PaintColorMultiplier", "MetalColorMultiplier")
SCALAR_KEYS = ("PaintWear", "PaintWearTightness", "DirtStrength", "GrimeMultiplier", "DirtRoughness", "PaintRoughness",
               "PaintRoughnessMultiplier", "ScratchRoughnessMultiplier", "GrimeRoughnessMultiplier", "MetalDetailContrast",
               "PaintDetailContrast", "MetalTilingScale", "PaintTilingScale")
METALS = ("Base Metal", "Gun Metal", "Black Metal", "Steel", "Aluminum", "Matte", "Anodized", "Sensor")
METAL_ID = {"000": "Base Metal", "010": "Gun Metal", "011": "Steel", "001": "Black Metal",
            "101": "Sensor", "100": "Anodized", "111": "Aluminum", "110": "Matte"}


def env(name):
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"set {name}")
    return value


def run(*cmd, ok=(0,)):
    print("+", " ".join(str(c) for c in cmd))
    argv = [sys.executable, IDLE, *map(str, cmd)] if Path(IDLE).is_file() else list(map(str, cmd))
    if subprocess.run(argv).returncode not in ok:
        raise SystemExit(f"failed: {cmd[0]}")


def native_request(recipe):
    ex = recipe["extraction"]
    return {"mesh": ex["mesh"], "clips": list(ex["clips"].values()), "materials": ex["materials"],
            "loadouts": [], "textures": list(ex["textures"].values())}


def material_params(materials, defaults):
    """Merge the shared material defaults with the instance chain (later entries win) into bake_material's parameter file."""
    scalars = dict(defaults["S"])
    vectors = {k: v[:3] for k, v in defaults["V"].items()}
    for entry in materials:
        scalars.update({s["ParameterInfo"]["Name"]: s["ParameterValue"] for s in entry.get("scalars", [])})
        vectors.update({v["ParameterInfo"]["Name"]: [v["ParameterValue"][c] for c in "RGB"] for v in entry.get("vectors", [])})
    return {"scalars": {k: scalars[k] for k in SCALAR_KEYS},
            "vectors": {"DirtColor": vectors["DirtColor"], "GrimeColor": vectors["GrimeColor"]},
            "paint": {k: vectors[k] for k in PAINT_KEYS},
            "metals": {m: {"roughness": scalars[m + " Roughness"], "metallic": scalars[m + " Metallic"]} for m in METALS},
            "metalId": METAL_ID}


def texture_files(native):
    """asset name -> PNG path, from the native extractor's textures.json."""
    rows = json.loads((Path(native) / "textures.json").read_text())
    return {r["asset"].rsplit(".", 1)[-1].lower(): Path(native) / r["file"] for r in rows}


def free_tile(glb, extra_uvs=(), size=24, atlas=2048, cell=4):
    """[x, y, size] of an atlas tile no triangle samples (12 px margin); the cockpit glass paints there.
    extra_uvs: (uv, triangles) of parts added later that share the atlas."""
    doc, buf = load_glb(glb)
    n = atlas // cell
    img = Image.new("L", (n, n), 0)
    draw = ImageDraw.Draw(img)
    sources = [(build_glb.read_accessor(doc, buf, pr["attributes"]["TEXCOORD_0"]),
                build_glb.read_accessor(doc, buf, pr["indices"]).astype(int).reshape(-1, 3))
               for mesh in doc["meshes"] for pr in mesh["primitives"]] + list(extra_uvs)
    for uv, tris in sources:
        for tri in uv[tris] * n:
            draw.polygon([tuple(c) for c in tri], fill=255)
    covered = np.asarray(img) > 0
    for tile, margin in ((size, 3), (16, 2), (8, 1)):   # a fully packed atlas gets a smaller tile and margin
        used = ndimage.binary_dilation(covered, iterations=margin)
        window = tile // cell
        free = np.argwhere(~ndimage.maximum_filter(used, size=window, mode="constant", cval=1))
        if len(free):
            cy, cx = free[len(free) // 2]
            return [int((cx - window // 2) * cell), int((cy - window // 2) * cell), tile]
    raise ValueError("no free atlas tile for the cockpit glass")


def extract(recipe, work):
    ex, native = recipe["extraction"], work / "native"
    request = work / "request.json"
    request.write_text(json.dumps(native_request(recipe), indent=1))
    game = Path(env("MW5_INSTALL"))
    if not (native / "bridge.json").is_file():   # the extractor refuses a non-empty output: delete --work/native to redo
        run("dotnet", env("MW5_EXTRACT_DLL"), "--install", game, "--paks", game / "MW5Mercs/Content/Paks",
            "--ue", "4.26", "--request", request, "--out", native, ok=(0, 2))  # 2 = assets left on the bridge
    work.joinpath("hull.gltf").write_text(json.dumps(from_native.convert(native), allow_nan=False))
    objects = [ex["cockpit"]["hull"]] + ([ex["cockpit"]["glass"]] if ex["cockpit"].get("glass") else []) + list(ex["weapons"].values())
    for path in objects:
        if not (work / "ex" / (path.lstrip("/") + ".gltf")).is_file():
            # umodel takes one exact object per call; a wildcard silently exports nothing
            run(env("UMODEL"), "-export", f"-path={game / 'MW5Mercs/Content/Paks'}", "-game=ue4.26", f"-out={work / 'ex'}",
                "-gltf", "-png", "-nooverwrite", path)


def weapon_attachments(recipe, work):
    ex = recipe["extraction"]
    out, seen = [], set()
    for row in recipe["loadout"]:
        if row.get("copy_of") or (row["mesh"], row["bone"]) in seen:
            continue
        seen.add((row["mesh"], row["bone"]))
        out.append(f"{work / 'ex' / (ex['weapons'][row['mesh']].lstrip('/') + '.gltf')}={row['bone']}")
    return out


def hull_glb(recipe, work):
    ex, stem = recipe["extraction"], recipe["mc2"]["stem"]
    base = work / "base.glb"
    build_glb.build(work / "hull.gltf", None, base, f"{stem}_alb.png", attachments=weapon_attachments(recipe, work), recipe=recipe)
    cockpit = ex["cockpit"]
    keep_glass = cockpit.get("glass") and not recipe["cockpit"].get("drop_glass")   # a glass cage can read as black wire
    glass = work / "ex" / (cockpit["glass"].lstrip("/") + ".gltf") if keep_glass else None
    hull = [(uv, tri) for _, _, uv, tri in add_cockpit.static_primitives(work / "ex" / (cockpit["hull"].lstrip("/") + ".gltf"))]
    recipe["cockpit"]["glass"]["tile"] = free_tile(base, hull)
    out = work / "cockpit.glb"
    add_cockpit.add_cockpit(base, out, work / "ex" / (cockpit["hull"].lstrip("/") + ".gltf"), glass, recipe)
    trimmed = work / "trim.glb"
    print("glass pieces dropped:", add_cockpit.trim_proud_glass(out, trimmed))
    prone_pose.rewrite(trimmed, work / "final.glb", recipe.get("prone_pitch", 85.0))


DIRT_STRENGTH = 1.5           # the approved Mad Cat look: MW5's 0.75 doubled
MADCAT_BOX = np.array([[-4.11, -0.09, -5.92], [4.66, 15.58, 5.92]])   # body bounds the DECALS were placed on


def scale_decals(placements, points):
    """Re-seat the Mad Cat decal placements on another chassis by its body bounds (x along the body, y height, z half-width)."""
    lo, hi = points.min(0), points.max(0)
    (rl, rh), height = MADCAT_BOX, (points[:, 1].max() - points[:, 1].min()) / (MADCAT_BOX[1, 1] - MADCAT_BOX[0, 1])
    out = []
    for d in placements:
        at = np.asarray(d["at"], float)
        t = (at - rl) / (rh - rl)
        out.append({**d, "at": tuple(lo + t * (hi - lo)), "size": tuple(s * height for s in d["size"]), "depth": d["depth"] * max(height, .6)})
    return out


def bake(recipe, work):
    native = work / "native"
    files = texture_files(native)
    ex = recipe["extraction"]
    leaf = lambda role: files[ex["textures"][role].rsplit(".", 1)[-1].lower()]  # noqa: E731
    defaults = json.loads(Path(env("MW5_MATERIAL_DEFAULTS")).read_text())
    params = material_params(json.loads((native / "materials.json").read_text()), defaults)
    size = bake_material.SIZE
    metal_id = np.zeros((size, size, 4), np.float32)
    wear = np.zeros((size, size, 4), np.float32)
    if "metal_id" in ex["textures"]:
        metal_id, wear = bake_material.load(leaf("metal_id")), bake_material.load(leaf("wear"))
    else:   # no authored wear map on this chassis: shared macro variation drives a light dirt layer
        wear[..., 1] = bake_material.load(leaf("macro"), channels="L")[..., 0] * 0.5
    glb = work / "final.glb"
    params["scalars"]["DirtStrength"] = recipe.get("dirt_strength", DIRT_STRENGTH)
    wear[..., 0] = np.maximum(wear[..., 0], bake_extras.edge_wear(glb))   # paint chips on convex edges
    detail = None
    if "castmetal_nrm" in files and "microvariaton_nrm" in files:
        detail = tuple(bake_material.load(files[k], channels="RGB") for k in ("castmetal_nrm", "microvariaton_nrm"))
    alb, orm, nrm = bake_material.bake(bake_material.load(leaf("mask")), metal_id, wear,
                                       bake_material.load(leaf("normal"), channels="RGB"), params, detail)
    out = work / "bake"
    out.mkdir(exist_ok=True)
    body = bake_extras.body_triangles(glb)
    fields = bake_extras.rasterize(body)
    a, o = bake_extras.weather(fields, np.asarray(alb), np.asarray(orm), leaf("rust"), leaf("macro"))
    if "decals" in ex["textures"]:
        a, o = bake_extras.decals(fields, a, o, leaf("decals"),
                                  scale_decals(bake_extras.DECALS, np.concatenate([p for p, _, _, _ in body])))
    Image.fromarray(a).save(out / "alb.png")
    Image.fromarray(o).save(out / "orm.png")
    nrm.save(out / "nrm.png")
    add_cockpit.stamp(out, recipe)


def pack(recipe, work, install, mod_id):
    stem = recipe["mc2"]["stem"]
    cooked = work / "cooked"
    cook_textures.cook(env("KTX"), work / "bake/alb.png", work / "bake/nrm.png", cooked, work / "scratch", recipe,
                       work / "bake/orm.png", True)
    (work / "loadout.json").write_text(json.dumps(recipe["loadout"]))
    recipe_path = work / "recipe.json"
    recipe_path.write_text(json.dumps(recipe))
    tgl = Path(install) / "mods" / (mod_id or recipe["mod"]["id"]) / "data" / "tgl"
    tgl.mkdir(parents=True, exist_ok=True)
    Image.open(work / "bake/alb.png").save(tgl / f"{stem}_alb.png")
    sys.argv = ["build_pack", "--install", str(install), "--recipe", str(recipe_path), "--glb", str(work / "final.glb"),
                "--textures", str(cooked), "--loadout", str(work / "loadout.json"), "--mod-id", mod_id or recipe["mod"]["id"]]
    build_pack.main()
    (tgl / f"{stem}.glb").write_bytes((work / "final.glb").read_bytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--recipe", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--install", type=Path, required=True)
    ap.add_argument("--steps", default="extract,glb,bake,pack")
    ap.add_argument("--mod-id")
    a = ap.parse_args()
    recipe = build_glb.load_recipe(a.recipe)
    a.work.mkdir(parents=True, exist_ok=True)
    steps = a.steps.split(",")
    if "extract" in steps:
        extract(recipe, a.work)
    if "glb" in steps:
        hull_glb(recipe, a.work)
        (a.work / "tile.json").write_text(json.dumps(recipe["cockpit"]["glass"]["tile"]))
    else:
        recipe["cockpit"]["glass"]["tile"] = json.loads((a.work / "tile.json").read_text())
    if "bake" in steps:
        bake(recipe, a.work)
    if "pack" in steps:
        pack(recipe, a.work, a.install, a.mod_id)


if __name__ == "__main__":
    main()

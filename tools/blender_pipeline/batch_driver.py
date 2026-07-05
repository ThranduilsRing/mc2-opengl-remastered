#!/usr/bin/env python3
"""
batch_driver.py — BLENDER-ASSET-PIPELINE-1 manifest-driven batch driver.

End-to-end headless asset upscale/reimagine over EXISTING game assets:

    extract  (stock .ase + .ini  ->  GLB, via tools/ase_to_glb.py)
    transform (headless Blender recipe: upscale_mesh | decimate_lods | rebake_pbr)
    patch    (carry stock texture URIs into the Blender-exported GLB)
    inject   (engine-consumable override: data/tgl <name>.ini [Import] Source=
              for buildings/trees/vehicles/props, or model_overrides/models.json
              LOD chains for the TREE-OVERRIDE-LOD sink)

Usage:
    py -3 tools/blender_pipeline/batch_driver.py MANIFEST.json [--dry-run]
        [--blender EXE] [--only JOB[,JOB..]] [--deploy-tgl-dir DIR]

Exit codes: 0 ok, 2 manifest/validation error, 1 stage failure.
Manifest schema: see manifest_schema.json + README.md in this directory.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blender_runner import build_blender_cmd, find_blender_exe, run_blender_script  # noqa: E402

TOOL_DIR = Path(__file__).resolve().parent
RECIPE_DIR = TOOL_DIR / "recipes"
ASE_TO_GLB = TOOL_DIR.parent / "ase_to_glb.py"

KNOWN_RECIPES = ("upscale_mesh", "decimate_lods", "rebake_pbr")
OUT_KINDS = ("tgl_ini", "models_json", "glb_only")
ASSET_CLASSES = ("mech", "prop", "building", "vehicle", "tree")
SCHEMA_VERSION = "1.0"


class ManifestError(Exception):
    def __init__(self, errors):
        self.errors = list(errors)
        super().__init__("\n".join(self.errors))


# ---------------------------------------------------------------------------
# Manifest load + validation
# ---------------------------------------------------------------------------

def validate_manifest(data, base_dir):
    """Return a list of error strings (empty = valid). Pure — pytest-covered."""
    errors = []
    if not isinstance(data, dict):
        return ["manifest root must be a JSON object"]
    if data.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION!r} "
                      f"(got {data.get('schema_version')!r})")
    jobs = data.get("jobs")
    if not isinstance(jobs, list) or not jobs:
        errors.append("jobs must be a non-empty list")
        return errors

    seen = set()
    for i, job in enumerate(jobs):
        tag = f"jobs[{i}]"
        if not isinstance(job, dict):
            errors.append(f"{tag}: must be an object")
            continue
        name = job.get("name")
        if not name or not isinstance(name, str):
            errors.append(f"{tag}: missing job name")
        elif name in seen:
            errors.append(f"{tag}: duplicate job name {name!r}")
        else:
            seen.add(name)
            tag = f"jobs[{i}] ({name})"

        recipe = job.get("recipe")
        if recipe not in KNOWN_RECIPES:
            errors.append(f"{tag}: recipe must be one of {KNOWN_RECIPES} "
                          f"(got {recipe!r})")

        cls = job.get("asset_class", "building")
        if cls not in ASSET_CLASSES:
            errors.append(f"{tag}: asset_class must be one of {ASSET_CLASSES}")

        src = job.get("source")
        if not isinstance(src, dict):
            errors.append(f"{tag}: missing source object")
        else:
            has_ase = "ase" in src
            has_glb = "glb" in src
            if has_ase == has_glb:
                errors.append(f"{tag}: source needs exactly one of 'ase' or 'glb'")
            if has_ase and "ini" not in src:
                errors.append(f"{tag}: source.ase requires source.ini")
            for key in ("ase", "ini", "glb"):
                if key in src:
                    p = _resolve(base_dir, src[key])
                    if not p.is_file():
                        errors.append(f"{tag}: source.{key} not found: {p}")

        out = job.get("out")
        if not isinstance(out, dict):
            errors.append(f"{tag}: missing out object")
        else:
            kind = out.get("kind")
            if kind not in OUT_KINDS:
                errors.append(f"{tag}: out.kind must be one of {OUT_KINDS} "
                              f"(got {kind!r})")
            if not out.get("dir"):
                errors.append(f"{tag}: out.dir is required")
            if kind == "tgl_ini":
                if not out.get("source_name"):
                    errors.append(f"{tag}: out.kind=tgl_ini requires out.source_name")
                if not (isinstance(src, dict) and "ini" in src):
                    errors.append(f"{tag}: out.kind=tgl_ini requires source.ini "
                                  "(stock ini is the override template)")
            if kind == "models_json":
                if recipe != "decimate_lods":
                    errors.append(f"{tag}: out.kind=models_json only pairs with "
                                  "recipe=decimate_lods")
                if out.get("override_class") not in ("staticprop", "tree"):
                    errors.append(f"{tag}: out.override_class must be "
                                  "'staticprop' or 'tree'")
                if not out.get("appearance_name"):
                    errors.append(f"{tag}: out.appearance_name is required")

        if recipe == "decimate_lods":
            params = job.get("params") or {}
            ratios = params.get("ratios")
            if not isinstance(ratios, list) or not ratios or \
                    not all(isinstance(r, (int, float)) and 0 < r <= 1 for r in ratios):
                errors.append(f"{tag}: decimate_lods needs params.ratios "
                              "(list of floats in (0,1])")
            distances = params.get("distances")
            if distances is not None and (
                    not isinstance(distances, list) or len(distances) != len(ratios or [])):
                errors.append(f"{tag}: params.distances must match params.ratios length")
    return errors


def load_manifest(path):
    path = Path(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError([f"cannot read manifest {path}: {exc}"])
    errors = validate_manifest(data, path.parent)
    if errors:
        raise ManifestError(errors)
    return data


def _resolve(base_dir, p):
    p = Path(p)
    return p if p.is_absolute() else (Path(base_dir) / p)


# ---------------------------------------------------------------------------
# Inject helpers (pure — pytest-covered)
# ---------------------------------------------------------------------------

def build_override_ini(stock_ini_text, source_name):
    """Insert (or replace) an [Import] block after the FITini header.

    Matches the proven data/tgl/quonset.ini pattern:
        FITini
        [Import]
        st Source = "QuonsetGLB"
        ...stock sections unchanged...
    Idempotent: an existing [Import] block is replaced.
    """
    lines = stock_ini_text.splitlines()
    out, skipping, inserted = [], False, False
    block = ["", "[Import]", f'st Source = "{source_name}"']

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            skipping = stripped.lower() == "[import]"
            if skipping:
                continue
        if skipping:
            if not stripped or stripped.startswith("//"):
                skipping = False  # blank/comment ends the removed block
                if stripped.startswith("//"):
                    out.append(line)
            continue
        out.append(line)
        if not inserted and stripped == "FITini":
            out.extend(block)
            inserted = True

    if not inserted:  # no FITini header (defensive) — prepend
        out = ["FITini"] + block + out
    return "\n".join(out) + "\n"


def build_models_json_entry(out_spec, lod_files, distances):
    """One ModelOverrideRecord dict in the exact shape the engine parses
    (mclib/model_override_registry.h — MODEL-OVERRIDE-MVP-1 invariants:
    renderOnly=true, fallback=stock, ascending lods, distance 0 = LOD0)."""
    cls = out_spec["override_class"]
    appearance = out_spec["appearance_name"]
    prefix = out_spec.get("source_prefix", "data/model_overrides/cooked").rstrip("/")
    lods = []
    for i, fname in enumerate(lod_files):
        lods.append({
            "distance": 0 if i == 0 else float(distances[i - 1]),
            "source": f"{prefix}/{fname}",
        })
    return {
        "class": cls,
        "appearanceName": appearance,
        "replaces": f"{cls}:{appearance}",
        "renderOnly": True,
        "fallback": "stock",
        "lods": lods,
    }


def merge_models_json(existing_entries, new_entry):
    """Replace-or-append by the 'replaces' key (engine rule: duplicate key —
    first wins — so we must not leave stale duplicates behind)."""
    key = new_entry["replaces"]
    merged = [e for e in existing_entries if e.get("replaces") != key]
    merged.append(new_entry)
    return merged


# ---------------------------------------------------------------------------
# GLB texture carry-over (pure — pytest-covered)
# ---------------------------------------------------------------------------

def read_glb(path):
    """Return (gltf_json_dict, bin_chunk_bytes_or_None)."""
    data = Path(path).read_bytes()
    magic, _ver, _total = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise ValueError(f"not a GLB: {path}")
    offset, gltf, binary = 12, None, None
    while offset < len(data):
        clen, ctype = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8: offset + 8 + clen]
        if ctype == 0x4E4F534A:
            gltf = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            binary = chunk
        offset += 8 + clen
    return gltf, binary


def write_glb(gltf, binary, path):
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    chunks = struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    if binary is not None:
        binary = binary + b"\x00" * (-len(binary) % 4)
        chunks += struct.pack("<II", len(binary), 0x004E4942) + binary
    header = struct.pack("<III", 0x46546C67, 2, 12 + len(chunks))
    Path(path).write_bytes(header + chunks)


def _base_mat_name(name):
    """Strip Blender's .001-style duplicate suffix."""
    if len(name) > 4 and name[-4] == "." and name[-3:].isdigit():
        return name[:-4]
    return name


def carry_textures(src_gltf, dst_gltf):
    """Copy URI-referenced baseColor textures from the source GLB into the
    destination GLB for materials (matched by name) that lost them in the
    Blender round-trip. Stock ASE->GLB textures are external URIs resolved by
    the engine's texture manager, so URI carry-over preserves texturing without
    needing the .tga on disk at cook time. Returns count of patched materials.
    """
    src_mats = {m.get("name", ""): m for m in src_gltf.get("materials", [])}
    src_textures = src_gltf.get("textures", [])
    src_images = src_gltf.get("images", [])
    patched = 0

    for mat in dst_gltf.get("materials", []):
        name = _base_mat_name(mat.get("name", ""))
        src = src_mats.get(name)
        if src is None:
            continue
        src_pbr = src.get("pbrMetallicRoughness", {})
        src_tex_ref = src_pbr.get("baseColorTexture")
        if src_tex_ref is None:
            continue
        dst_pbr = mat.setdefault("pbrMetallicRoughness", {})
        if "baseColorTexture" in dst_pbr:
            continue  # Blender kept/rebaked a texture — don't clobber
        src_img = src_images[src_textures[src_tex_ref["index"]]["source"]]
        uri = src_img.get("uri")
        if not uri:
            continue  # embedded (bufferView) images not carried in v1
        images = dst_gltf.setdefault("images", [])
        textures = dst_gltf.setdefault("textures", [])
        img_idx = next((i for i, im in enumerate(images) if im.get("uri") == uri), None)
        if img_idx is None:
            images.append({"uri": uri})
            img_idx = len(images) - 1
        tex_idx = next((i for i, t in enumerate(textures)
                        if t.get("source") == img_idx), None)
        if tex_idx is None:
            textures.append({"source": img_idx})
            tex_idx = len(textures) - 1
        dst_pbr["baseColorTexture"] = {"index": tex_idx}
        patched += 1
    return patched


# ---------------------------------------------------------------------------
# Planning (pure — pytest-covered)
# ---------------------------------------------------------------------------

def recipe_args_for(job, ctx):
    """Build the --k=v dict handed to the recipe inside Blender."""
    name = job["name"]
    work = ctx["work_dir"] / name
    params = dict(job.get("params") or {})
    recipe = job["recipe"]
    args = {}

    if recipe == "upscale_mesh":
        args["in"] = str(work / "src.glb")
        args["out"] = str(work / "transformed.glb")
        for k in ("subdiv", "subdiv-type", "weld", "crease-angle", "shade-smooth"):
            pk = k.replace("-", "_")
            if pk in params:
                args[k] = params[pk]
    elif recipe == "decimate_lods":
        args["in"] = str(work / "src.glb")
        args["out-dir"] = str(work / "lods")
        args["name"] = job["out"].get("appearance_name") or \
            job["out"].get("source_name") or name
        args["ratios"] = ",".join(str(r) for r in params["ratios"])
        for k in ("weld", "min-tris"):
            pk = k.replace("-", "_")
            if pk in params:
                args[k] = params[pk]
    elif recipe == "rebake_pbr":
        args["in"] = str(work / "src.glb")
        args["out"] = str(work / "transformed.glb")
        args["tex-dir"] = str(work / "tex")
        args["name"] = job["out"].get("source_name") or name
        for k in ("res", "samples", "margin", "uv"):
            if k in params:
                args[k] = params[k]
    return args


def plan_job(job, ctx):
    """Return the ordered stage list for one job (no side effects)."""
    name = job["name"]
    work = ctx["work_dir"] / name
    base = ctx["base_dir"]
    src = job["source"]
    stages = []

    if "ase" in src:
        stages.append({
            "stage": "extract",
            "cmd": [sys.executable, str(ASE_TO_GLB),
                    "--ase", str(_resolve(base, src["ase"])),
                    "--ini", str(_resolve(base, src["ini"])),
                    "--out-glb", str(work / "src.glb"),
                    "--out-sidecar", str(work / "src.mcasset.json"),
                    "--asset-class", job.get("asset_class", "building")],
        })
    else:
        stages.append({
            "stage": "extract",
            "copy": [str(_resolve(base, src["glb"])), str(work / "src.glb")],
        })

    recipe_script = RECIPE_DIR / f"{job['recipe']}.py"
    stages.append({
        "stage": "transform",
        "recipe": job["recipe"],
        "script": str(recipe_script),
        "args": recipe_args_for(job, ctx),
    })

    out = job["out"]
    if out["kind"] == "tgl_ini" and out.get("carry_textures", True):
        stages.append({"stage": "patch_textures",
                       "src_glb": str(work / "src.glb"),
                       "dst_glb": str(work / "transformed.glb")})

    stages.append({"stage": "inject", "kind": out["kind"],
                   "out_dir": str(_resolve(base, out["dir"]))})
    return stages


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

def _run_stage_extract(stage):
    if "copy" in stage:
        src, dst = stage["copy"]
        Path(dst).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        return {}
    Path(stage["cmd"][stage["cmd"].index("--out-glb") + 1]).parent.mkdir(
        parents=True, exist_ok=True)
    proc = subprocess.run(stage["cmd"], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"extract failed:\n{proc.stdout[-1500:]}\n{proc.stderr[-1500:]}")
    return {}


def _run_stage_transform(stage, job, ctx):
    log = ctx["work_dir"] / job["name"] / f"blender_{job['recipe']}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    stdout = run_blender_script(ctx["blender_exe"], stage["script"],
                                stage["args"], log_path=str(log))
    stats = {}
    for line in stdout.splitlines():
        if line.startswith("[recipe:stats:"):
            tag, _, payload = line.partition("] ")
            try:
                stats[tag.split(":")[-1]] = json.loads(payload)
            except json.JSONDecodeError:
                pass
    return {"stats": stats, "log": str(log)}


def _run_stage_patch(stage):
    src_gltf, _ = read_glb(stage["src_glb"])
    dst_gltf, dst_bin = read_glb(stage["dst_glb"])
    patched = carry_textures(src_gltf, dst_gltf)
    if patched:
        write_glb(dst_gltf, dst_bin, stage["dst_glb"])
    return {"patched_materials": patched}


def _run_stage_inject(stage, job, ctx):
    out = job["out"]
    work = ctx["work_dir"] / job["name"]
    out_dir = Path(stage["out_dir"])
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []

    if out["kind"] == "glb_only":
        dst = out_dir / f"{out.get('source_name', job['name'])}.glb"
        shutil.copyfile(work / "transformed.glb", dst)
        written.append(str(dst))

    elif out["kind"] == "tgl_ini":
        source_name = out["source_name"]
        glb_dst = out_dir / f"{source_name}.glb"
        shutil.copyfile(work / "transformed.glb", glb_dst)
        written.append(str(glb_dst))

        stock_ini = _resolve(ctx["base_dir"], job["source"]["ini"])
        ini_text = build_override_ini(
            stock_ini.read_text(encoding="utf-8", errors="replace"), source_name)
        ini_dst = out_dir / stock_ini.name
        ini_dst.write_text(ini_text, encoding="utf-8")
        written.append(str(ini_dst))

        sidecar = work / "src.mcasset.json"
        if sidecar.is_file():
            sc_dst = out_dir / f"{source_name}.mcasset.json"
            shutil.copyfile(sidecar, sc_dst)
            written.append(str(sc_dst))

        tex_dir = work / "tex"
        if tex_dir.is_dir():
            for png in sorted(tex_dir.glob("*.png")):
                shutil.copyfile(png, out_dir / png.name)
                written.append(str(out_dir / png.name))

        if ctx.get("deploy_tgl_dir"):
            deploy = Path(ctx["deploy_tgl_dir"])
            deploy.mkdir(parents=True, exist_ok=True)
            for w in list(written):
                if Path(w).suffix in (".glb", ".ini", ".json"):
                    shutil.copyfile(w, deploy / Path(w).name)
                    written.append(str(deploy / Path(w).name))

    elif out["kind"] == "models_json":
        lod_dir = work / "lods"
        base_name = out["appearance_name"]
        lod0 = out_dir / f"{base_name}_lod0.glb"
        shutil.copyfile(work / "src.glb", lod0)  # LOD0 = untouched source
        lod_files = [lod0.name]
        written.append(str(lod0))
        for glb in sorted(lod_dir.glob(f"{base_name}_lod*.glb")):
            dst = out_dir / glb.name
            shutil.copyfile(glb, dst)
            lod_files.append(dst.name)
            written.append(str(dst))

        params = job.get("params") or {}
        ratios = params["ratios"]
        distances = params.get("distances") or \
            [600.0 * (i + 1) for i in range(len(ratios))]
        entry = build_models_json_entry(out, lod_files, distances)

        manifest_path = out_dir / "models.json"
        existing = []
        if manifest_path.is_file():
            try:
                existing = json.loads(manifest_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                existing = []
        merged = merge_models_json(existing, entry)
        manifest_path.write_text(json.dumps(merged, indent=2) + "\n",
                                 encoding="utf-8")
        written.append(str(manifest_path))

    return {"written": written}


def run_job(job, ctx):
    stages = plan_job(job, ctx)
    results = []
    for stage in stages:
        t0 = time.time()
        kind = stage["stage"]
        if kind == "extract":
            info = _run_stage_extract(stage)
        elif kind == "transform":
            info = _run_stage_transform(stage, job, ctx)
        elif kind == "patch_textures":
            info = _run_stage_patch(stage)
        elif kind == "inject":
            info = _run_stage_inject(stage, job, ctx)
        else:  # pragma: no cover — plan_job only emits the four above
            raise RuntimeError(f"unknown stage {kind}")
        info.update({"stage": kind, "seconds": round(time.time() - t0, 2)})
        results.append(info)
        print(f"  [{job['name']}] {kind} ok ({info['seconds']}s)")
    return results


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("manifest", help="pipeline manifest JSON")
    p.add_argument("--dry-run", action="store_true",
                   help="validate + print the plan; run nothing")
    p.add_argument("--blender", help="explicit blender.exe path "
                   "(else BLENDER_EXECUTABLE env, then known installs)")
    p.add_argument("--only", help="comma-separated job names to run")
    p.add_argument("--deploy-tgl-dir",
                   help="also copy tgl_ini outputs into this deploy data/tgl dir")
    args = p.parse_args(argv)

    try:
        manifest = load_manifest(args.manifest)
    except ManifestError as exc:
        print("MANIFEST INVALID:", file=sys.stderr)
        for e in exc.errors:
            print(f"  - {e}", file=sys.stderr)
        return 2

    base_dir = Path(args.manifest).resolve().parent
    work_dir = _resolve(base_dir, manifest.get("work_dir", "_work"))
    jobs = manifest["jobs"]
    if args.only:
        wanted = {j.strip() for j in args.only.split(",")}
        unknown = wanted - {j["name"] for j in jobs}
        if unknown:
            print(f"--only names not in manifest: {sorted(unknown)}", file=sys.stderr)
            return 2
        jobs = [j for j in jobs if j["name"] in wanted]

    ctx = {"base_dir": base_dir, "work_dir": Path(work_dir),
           "deploy_tgl_dir": args.deploy_tgl_dir or manifest.get("deploy_tgl_dir")}

    if args.dry_run:
        print(f"DRY RUN — {len(jobs)} job(s)")
        try:
            exe = find_blender_exe(args.blender or manifest.get("blender_exe"))
            print(f"blender: {exe}")
        except RuntimeError as exc:
            print(f"blender: NOT FOUND (transform stages would fail)\n  {exc}")
        for job in jobs:
            print(f"\njob {job['name']} (recipe={job['recipe']}):")
            for stage in plan_job(job, ctx):
                if "cmd" in stage:
                    detail = " ".join(stage["cmd"])
                elif "copy" in stage:
                    detail = f"copy {stage['copy'][0]} -> {stage['copy'][1]}"
                elif stage["stage"] == "transform":
                    detail = " ".join(build_blender_cmd("<blender>", stage["script"],
                                                        stage["args"]))
                else:
                    detail = json.dumps({k: v for k, v in stage.items()
                                         if k != "stage"})
                print(f"  {stage['stage']}: {detail}")
        return 0

    try:
        ctx["blender_exe"] = find_blender_exe(args.blender or manifest.get("blender_exe"))
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    report = {"manifest": str(Path(args.manifest).resolve()),
              "blender": str(ctx["blender_exe"]), "jobs": {}}
    failed = False
    for job in jobs:
        print(f"job {job['name']} (recipe={job['recipe']}):")
        try:
            report["jobs"][job["name"]] = {"ok": True, "stages": run_job(job, ctx)}
        except (RuntimeError, OSError, ValueError) as exc:
            print(f"  [{job['name']}] FAILED: {exc}", file=sys.stderr)
            report["jobs"][job["name"]] = {"ok": False, "error": str(exc)}
            failed = True

    ctx["work_dir"].mkdir(parents=True, exist_ok=True)
    report_path = ctx["work_dir"] / "pipeline_report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"report: {report_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

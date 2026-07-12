#!/usr/bin/env python3
"""
asset_hd.py — BLENDER-ASSET-HD-CLI-1 one-command per-asset upscale.

Thin front-end over batch_driver.py: resolve stock asset names against the
mc2 srcdata tgl directory, synthesize a single-run manifest in memory, and
hand it to the existing batch machinery (extract -> transform -> patch ->
inject). No manifest hand-authoring needed for the common case:

    py -3 tools/blender_pipeline/asset_hd.py quonset
    py -3 tools/blender_pipeline/asset_hd.py quonset --recipe decimate_lods \
        --out-kind models_json --param ratios=0.5,0.2
    py -3 tools/blender_pipeline/asset_hd.py --list quon

The synthesized manifest is written under <out>/_synth/ for inspection and
re-running via batch_driver.py directly.

Exit codes mirror batch_driver: 0 ok, 2 resolution/validation error,
1 stage failure.
"""
import argparse
import difflib
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batch_driver as bd  # noqa: E402

TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]
DEFAULT_OUT_BASE = TOOL_DIR / "out"
DEFAULT_LOD_RATIOS = (0.5, 0.2)

# Stock srcdata layout drifted across checkouts (mc2srcdata inside the repo
# vs the mc2-srcdata sibling next to it) — probe both, first hit wins.
SRCDATA_PROBES = (
    REPO_ROOT / "mc2srcdata",
    REPO_ROOT / "mc2-srcdata",
    REPO_ROOT.parent / "mc2-srcdata",
    REPO_ROOT.parent / "mc2srcdata",
)


class AssetResolutionError(Exception):
    pass


# ---------------------------------------------------------------------------
# Asset resolution (pure — pytest-covered)
# ---------------------------------------------------------------------------

def find_tgl_dir(explicit=None):
    """Return the stock tgl/ dir holding *.ase + *.ini, or raise RuntimeError.

    `explicit` may point at the srcdata root (containing tgl/) or at the tgl
    dir itself.
    """
    if explicit:
        p = Path(explicit)
        if (p / "tgl").is_dir():
            return p / "tgl"
        if p.is_dir():
            return p
        raise RuntimeError(f"--srcdata-root {explicit!r} is not a directory")

    for cand in SRCDATA_PROBES:
        if (cand / "tgl").is_dir():
            return cand / "tgl"
    raise RuntimeError(
        "Could not locate the stock srcdata tgl dir. Pass --srcdata-root. Probed:\n  "
        + "\n  ".join(str(c) for c in SRCDATA_PROBES))


def list_assets(tgl_dir, substring=""):
    """Sorted asset stems whose name contains `substring` (case-insensitive)."""
    want = substring.lower()
    return sorted((p.stem for p in Path(tgl_dir).glob("*.ase")
                   if want in p.stem.lower()), key=str.lower)


def resolve_asset(name, tgl_dir):
    """Return (stem, ase_path, ini_path) for a case-insensitive asset name.

    Raises AssetResolutionError with closest-match suggestions when the asset
    (or its mandatory stock .ini) is missing.
    """
    tgl_dir = Path(tgl_dir)
    want = name.lower()
    if want.endswith(".ase"):
        want = want[:-4]

    by_lower = {}
    for p in sorted(tgl_dir.glob("*.ase")):
        by_lower.setdefault(p.stem.lower(), p)
    ase = by_lower.get(want)
    if ase is None:
        stems = sorted(by_lower)
        close = difflib.get_close_matches(want, stems, n=6, cutoff=0.6)
        contains = [s for s in stems if want in s][:6]
        sugg = list(dict.fromkeys(close + contains))[:8]
        hint = f"; closest: {', '.join(sugg)}" if sugg else ""
        raise AssetResolutionError(
            f"asset {name!r} not found in {tgl_dir}{hint}")

    ini = {p.stem.lower(): p for p in tgl_dir.glob("*.ini")}.get(want)
    if ini is None:
        raise AssetResolutionError(
            f"asset {name!r}: no matching .ini for {ase.name} in {tgl_dir} "
            "(the ase->glb extractor needs the stock ini)")
    return ase.stem, ase, ini


# ---------------------------------------------------------------------------
# Param parsing + manifest synthesis (pure — pytest-covered)
# ---------------------------------------------------------------------------

def _scalar(raw):
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        pass
    return raw


def parse_param_value(raw):
    """'2' -> 2, '0.5' -> 0.5, 'true' -> True, '0.5,0.2' -> [0.5, 0.2]."""
    low = raw.lower()
    if low in ("true", "yes", "on"):
        return True
    if low in ("false", "no", "off"):
        return False
    if "," in raw:
        parts = [_scalar(x.strip()) for x in raw.split(",") if x.strip()]
        if parts and all(isinstance(x, (int, float)) for x in parts):
            return parts
        return raw
    return _scalar(raw)


def parse_params(items):
    """Parse repeated --param KEY=VALUE into a recipe params dict.

    Keys are normalized to underscore form (crease-angle -> crease_angle) to
    match the manifest schema; batch_driver maps them back to recipe flags.
    """
    params = {}
    for item in items:
        key, sep, raw = item.partition("=")
        key = key.strip()
        if not sep or not key:
            raise ValueError(f"--param needs KEY=VALUE (got {item!r})")
        params[key.replace("-", "_")] = parse_param_value(raw.strip())
    return params


def default_source_name(stem):
    """quonset -> QuonsetHD (the proven [Import] Source= naming)."""
    return stem[:1].upper() + stem[1:] + "HD"


def synthesize_manifest(resolved, recipe, params, out_kind, out_dirs, *,
                        asset_class="building", source_name=None,
                        override_class="staticprop", work_dir=None):
    """Build the batch_driver manifest dict for resolved (stem, ase, ini)
    triples. Pure — validation stays batch_driver's job."""
    jobs = []
    for stem, ase, ini in resolved:
        job_params = dict(params)
        if recipe == "decimate_lods" and "ratios" not in job_params:
            job_params["ratios"] = list(DEFAULT_LOD_RATIOS)
        out = {"kind": out_kind, "dir": str(out_dirs[stem])}
        if out_kind in ("tgl_ini", "glb_only"):
            out["source_name"] = source_name or default_source_name(stem)
        if out_kind == "models_json":
            out["override_class"] = override_class
            out["appearance_name"] = stem.lower()
        jobs.append({
            "name": f"{stem.lower()}_{recipe}",
            "recipe": recipe,
            "asset_class": asset_class,
            "source": {"ase": str(ase), "ini": str(ini)},
            "params": job_params,
            "out": out,
        })
    manifest = {"schema_version": bd.SCHEMA_VERSION, "jobs": jobs}
    if work_dir:
        manifest["work_dir"] = str(work_dir)
    return manifest


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _print_summary(report_path):
    """Compact post-run summary from batch_driver's pipeline_report.json."""
    try:
        report = json.loads(Path(report_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    for name, job in report.get("jobs", {}).items():
        if not job.get("ok"):
            continue
        saw_stats = False
        for stage in job.get("stages", []):
            for tag, s in (stage.get("stats") or {}).items():
                if isinstance(s, dict) and "tris_before" in s:
                    saw_stats = True
                    print(f"[asset_hd] {name}: {tag} "
                          f"tris {s['tris_before']} -> {s['tris_after']}, "
                          f"verts {s['verts_before']} -> {s['verts_after']}")
                    if s["tris_before"] == s["tris_after"]:
                        print(f"[asset_hd] WARNING: {name}: tri count unchanged "
                              f"— recipe may have been a no-op")
            if stage.get("stage") == "inject":
                for w in stage.get("written", []):
                    print(f"[asset_hd]   wrote {w}")
        if not saw_stats:
            print(f"[asset_hd] WARNING: {name}: no transform stats in report — "
                  f"cannot confirm geometry actually changed")


def main(argv=None):
    p = argparse.ArgumentParser(
        description="one-command per-asset upscale (front-end for batch_driver.py)")
    p.add_argument("assets", nargs="*", metavar="ASSET",
                   help="stock asset name(s), case-insensitive, resolved "
                        "against the srcdata tgl/*.ase inventory")
    p.add_argument("--recipe", default="upscale_mesh", choices=bd.KNOWN_RECIPES)
    p.add_argument("--param", action="append", default=[], metavar="KEY=VALUE",
                   help="recipe param (repeatable); ints/floats/bools/comma-"
                        "lists are parsed (e.g. --param subdiv=2)")
    p.add_argument("--out-kind", default="tgl_ini", choices=bd.OUT_KINDS)
    p.add_argument("--out-dir",
                   help="output dir (single asset: exact dir; multiple: "
                        "<dir>/<asset>/); default tools/blender_pipeline/out/<asset>/")
    p.add_argument("--deploy-tgl-dir",
                   help="also copy tgl_ini outputs into this deploy data/tgl "
                        "dir (no default — deploys are opt-in)")
    p.add_argument("--dry-run", action="store_true",
                   help="validate + print the plan; run nothing")
    p.add_argument("--list", nargs="?", const="", default=None,
                   metavar="SUBSTRING",
                   help="list available asset names (optionally filtered) and exit")
    p.add_argument("--srcdata-root",
                   help="stock srcdata root (or tgl dir); default probes "
                        "known checkouts")
    p.add_argument("--blender", help="explicit blender.exe path (passthrough)")
    p.add_argument("--asset-class", default="building", choices=bd.ASSET_CLASSES)
    p.add_argument("--source-name",
                   help="override the [Import] Source= name (single asset only; "
                        "default <Stem>HD)")
    p.add_argument("--override-class", default="staticprop",
                   choices=("staticprop", "tree"),
                   help="models_json override class")
    args = p.parse_args(argv)

    try:
        tgl_dir = find_tgl_dir(args.srcdata_root)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if args.list is not None:
        for stem in list_assets(tgl_dir, args.list):
            print(stem)
        return 0

    if not args.assets:
        print("ERROR: at least one ASSET name is required (or use --list)",
              file=sys.stderr)
        return 2
    if args.source_name and len(args.assets) > 1:
        print("ERROR: --source-name only applies to a single asset",
              file=sys.stderr)
        return 2

    try:
        resolved = [resolve_asset(n, tgl_dir) for n in args.assets]
        params = parse_params(args.param)
    except (AssetResolutionError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    out_base = Path(args.out_dir).resolve() if args.out_dir else DEFAULT_OUT_BASE
    single_exact = bool(args.out_dir) and len(resolved) == 1
    out_dirs = {stem: (out_base if single_exact else out_base / stem.lower())
                for stem, _, _ in resolved}

    synth_dir = out_base / "_synth"
    manifest = synthesize_manifest(
        resolved, args.recipe, params, args.out_kind, out_dirs,
        asset_class=args.asset_class, source_name=args.source_name,
        override_class=args.override_class, work_dir=synth_dir / "_work")

    synth_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = synth_dir / f"asset_hd_{args.recipe}.manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8")
    print(f"[asset_hd] manifest: {manifest_path}")

    forward = [str(manifest_path)]
    if args.dry_run:
        forward.append("--dry-run")
    if args.blender:
        forward += ["--blender", args.blender]
    if args.deploy_tgl_dir:
        forward += ["--deploy-tgl-dir", args.deploy_tgl_dir]
    rc = bd.main(forward)

    if rc == 0 and not args.dry_run:
        _print_summary(Path(manifest["work_dir"]) / "pipeline_report.json")
    return rc


if __name__ == "__main__":
    sys.exit(main())

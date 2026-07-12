#!/usr/bin/env python3
"""
sweep.py — BLENDER-ASSET-SWEEP-1 bulk category sweep over stock assets.

Enumerates stock `mc2srcdata/tgl/*.ase` assets that have a matching `.ini`
(the base-asset pairs — damage/LOD variants without an ini are skipped),
auto-generates a batch_driver manifest, and runs it through the existing
batch_driver machinery — sharded across N concurrent Blender processes.

Usage:
    # plan only (default): generate manifest + print summary, run nothing
    py -3 tools/blender_pipeline/sweep.py --filter "*" --limit 10

    # dry mode: write the generated manifest somewhere and exit
    py -3 tools/blender_pipeline/sweep.py --filter "tree*" --manifest-out t.json

    # execute (4 concurrent Blender processes)
    py -3 tools/blender_pipeline/sweep.py --filter "*" --limit 10 \
        --recipe upscale_mesh --jobs 4 --run

Report: `report.md` + `report.csv` in the out dir — per asset: source tris ->
output tris, duration, ok/fail + error line. Failures never abort the sweep.

Exit codes: 0 ok, 2 usage/enumeration/manifest error, 1 one or more jobs failed.
"""
import argparse
import concurrent.futures
import csv
import fnmatch
import json
import os
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batch_driver as bd  # noqa: E402
from blender_runner import find_blender_exe  # noqa: E402

TOOL_DIR = Path(__file__).resolve().parent

# Stock asset root probe order (README: mc2srcdata/tgl holds all 2947 .ase).
_SRCDATA_PROBES = [
    TOOL_DIR.parents[1] / "mc2srcdata" / "tgl",       # repo-local checkout
    Path("A:/Games/mc2-srcdata/tgl"),                 # canonical machine layout
    Path("A:/Games/mc2-opengl-src/mc2srcdata/tgl"),   # legacy sibling checkout
]

_PRINT_LOCK = threading.Lock()


# ---------------------------------------------------------------------------
# Enumeration + filtering (pure — pytest-covered)
# ---------------------------------------------------------------------------

def find_srcdata(explicit=None):
    """Return the stock tgl dir or raise RuntimeError with actionable text."""
    if explicit:
        p = Path(explicit)
        if p.is_dir():
            return p
        raise RuntimeError(f"--srcdata {explicit!r} is not a directory.")
    for cand in _SRCDATA_PROBES:
        if cand.is_dir():
            return cand
    raise RuntimeError(
        "Could not locate the stock mc2srcdata/tgl directory. Pass --srcdata. "
        "Probed:\n  " + "\n  ".join(str(p) for p in _SRCDATA_PROBES))


def enumerate_assets(tgl_dir):
    """Return sorted [(stem, ase_path, ini_path)] for every .ase with a
    matching .ini (case-insensitive stem match — Windows filesystem reality).
    """
    tgl_dir = Path(tgl_dir)
    inis = {p.stem.lower(): p for p in tgl_dir.glob("*.ini")}
    out = []
    for ase in tgl_dir.glob("*.ase"):
        ini = inis.get(ase.stem.lower())
        if ini is not None:
            out.append((ase.stem, ase, ini))
    out.sort(key=lambda t: t[0].lower())
    return out


def apply_filters(assets, filters, excludes, limit):
    """Glob-match asset stems (case-insensitive). limit<=0 means unlimited."""
    filters = [f.lower() for f in (filters or ["*"])]
    excludes = [e.lower() for e in (excludes or [])]
    picked = []
    for stem, ase, ini in assets:
        s = stem.lower()
        if not any(fnmatch.fnmatchcase(s, f) for f in filters):
            continue
        if any(fnmatch.fnmatchcase(s, e) for e in excludes):
            continue
        picked.append((stem, ase, ini))
        if limit > 0 and len(picked) >= limit:
            break
    return picked


def parse_param(text):
    """'key=value' -> (key, coerced value). JSON coercion, string fallback."""
    key, sep, raw = text.partition("=")
    if not sep or not key:
        raise ValueError(f"--param needs key=value (got {text!r})")
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        value = raw
    return key, value


# ---------------------------------------------------------------------------
# Manifest generation (pure — pytest-covered)
# ---------------------------------------------------------------------------

def build_manifest(assets, out_dir, recipe="upscale_mesh", params=None,
                   out_kind="tgl_ini", asset_class="building",
                   override_class="staticprop"):
    """Build a batch_driver schema-1.0 manifest dict for the picked assets.

    All paths are absolute so the manifest is location-independent (it can be
    written anywhere via --manifest-out and still validate/run).
    """
    out_dir = Path(out_dir)
    jobs = []
    for stem, ase, ini in assets:
        out = {"kind": out_kind,
               "dir": (out_dir / "assets" / stem).as_posix()}
        if out_kind == "tgl_ini":
            out["source_name"] = f"{stem}HD"
        elif out_kind == "models_json":
            out["override_class"] = override_class
            out["appearance_name"] = stem
        elif out_kind == "glb_only":
            out["source_name"] = stem
        job = {
            "name": stem,
            "recipe": recipe,
            "asset_class": asset_class,
            "source": {"ase": Path(ase).as_posix(),
                       "ini": Path(ini).as_posix()},
            "out": out,
        }
        if params:
            job["params"] = dict(params)
        jobs.append(job)
    return {
        "schema_version": bd.SCHEMA_VERSION,
        "work_dir": (out_dir / "_work").as_posix(),
        "jobs": jobs,
    }


def shard_jobs(jobs, n):
    """Round-robin the job list into <=n non-empty shards (deterministic)."""
    n = max(1, int(n))
    shards = [jobs[i::n] for i in range(n)]
    return [s for s in shards if s]


# ---------------------------------------------------------------------------
# Execution (per-job batch_driver.run_job calls on a shard-per-thread pool)
# ---------------------------------------------------------------------------

def _run_one(job, ctx):
    """run one job via batch_driver; never raises — collect the failure."""
    t0 = time.time()
    try:
        stages = bd.run_job(job, ctx)
        return {"name": job["name"], "ok": True, "stages": stages,
                "seconds": round(time.time() - t0, 2)}
    except (RuntimeError, OSError, ValueError) as exc:  # same set batch_driver traps
        with _PRINT_LOCK:
            print(f"  [{job['name']}] FAILED: {str(exc).splitlines()[0]}",
                  file=sys.stderr)
        return {"name": job["name"], "ok": False, "error": str(exc),
                "seconds": round(time.time() - t0, 2)}


def run_sweep(manifest, ctx, n_jobs):
    """Execute all manifest jobs, n_jobs shards concurrently. Returns results
    in the original job order (failures included, sweep never aborts)."""
    jobs = manifest["jobs"]
    shards = shard_jobs(jobs, n_jobs)
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(shards)) as pool:
        def run_shard(shard):
            return [_run_one(job, ctx) for job in shard]
        for shard_results in pool.map(run_shard, shards):
            for r in shard_results:
                results[r["name"]] = r
    return [results[j["name"]] for j in jobs]


# ---------------------------------------------------------------------------
# Report (pure — pytest-covered)
# ---------------------------------------------------------------------------

def tris_from_result(result):
    """(tris_before, tris_after) scraped from recipe stats; None if absent."""
    for stage in result.get("stages", []):
        if stage.get("stage") != "transform":
            continue
        for payload in (stage.get("stats") or {}).values():
            if "tris_before" in payload or "tris_after" in payload:
                return payload.get("tris_before"), payload.get("tris_after")
            lods = payload.get("lods")
            if isinstance(lods, list) and lods:
                tris = [l.get("tris") for l in lods if isinstance(l, dict)]
                tris = [t for t in tris if t is not None]
                return None, (tris[-1] if tris else None)
    return None, None


def _error_line(result):
    err = (result.get("error") or "").strip()
    return err.splitlines()[0] if err else ""


def write_report(results, out_dir, wall_seconds=None):
    """Write report.md + report.csv. Returns (ok_count, fail_count)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for r in results:
        before, after = tris_from_result(r)
        if not r["ok"]:
            status = "FAIL"
        elif before is None and after is None:
            # ok:true but no recipe stats — cannot prove the transform did
            # anything; surface it instead of a silently green row.
            status = "ok(no-stats)"
        elif before is not None and before == after:
            status = "ok(NO-CHANGE)"
        else:
            status = "ok"
        rows.append({
            "asset": r["name"],
            "status": status,
            "tris_before": "" if before is None else before,
            "tris_after": "" if after is None else after,
            "seconds": r.get("seconds", ""),
            "error": _error_line(r),
        })

    fields = ["asset", "status", "tris_before", "tris_after", "seconds", "error"]
    with open(out_dir / "report.csv", "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)

    ok = sum(1 for r in results if r["ok"])
    fail = len(results) - ok
    lines = ["# Sweep report", ""]
    lines.append(f"- jobs: {len(results)} — ok {ok}, failed {fail}")
    if wall_seconds is not None:
        lines.append(f"- wall time: {wall_seconds:.1f}s")
    lines += ["", "| asset | status | tris before | tris after | seconds | error |",
              "|---|---|---|---|---|---|"]
    for row in rows:
        lines.append("| {asset} | {status} | {tris_before} | {tris_after} "
                     "| {seconds} | {error} |".format(**row))
    (out_dir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return ok, fail


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("--srcdata", help="stock tgl dir (default: probe known roots)")
    p.add_argument("--filter", action="append", default=[], metavar="GLOB",
                   help="asset-stem glob, repeatable (default '*')")
    p.add_argument("--exclude", action="append", default=[], metavar="GLOB",
                   help="asset-stem glob to skip, repeatable")
    p.add_argument("--limit", type=int, default=10,
                   help="max assets (default 10 — safety; 0 = unlimited)")
    p.add_argument("--recipe", default="upscale_mesh", choices=bd.KNOWN_RECIPES)
    p.add_argument("--param", action="append", default=[], metavar="KEY=VALUE",
                   help="recipe param, repeatable (JSON-coerced values)")
    p.add_argument("--asset-class", default="building", choices=bd.ASSET_CLASSES)
    p.add_argument("--out-kind", default="tgl_ini", choices=bd.OUT_KINDS)
    p.add_argument("--override-class", default="staticprop",
                   choices=("staticprop", "tree"),
                   help="models_json only: engine override class")
    p.add_argument("--name", help="sweep name (default: recipe name); "
                   "out dir defaults to out/sweep_<name>/")
    p.add_argument("--out-dir", help="output dir "
                   "(default tools/blender_pipeline/out/sweep_<name>/)")
    p.add_argument("--jobs", type=int, default=4,
                   help="concurrent Blender processes (default 4)")
    p.add_argument("--manifest-out", metavar="PATH",
                   help="write the generated manifest to PATH and exit (dry mode)")
    p.add_argument("--run", action="store_true",
                   help="execute the sweep (default: plan + manifest only)")
    p.add_argument("--blender", help="explicit blender.exe path")
    p.add_argument("--deploy-tgl-dir",
                   help="also mirror tgl_ini outputs into this deploy data/tgl dir")
    args = p.parse_args(argv)

    try:
        params = dict(parse_param(s) for s in args.param)
        tgl_dir = find_srcdata(args.srcdata)
    except (RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    candidates = enumerate_assets(tgl_dir)
    picked = apply_filters(candidates, args.filter, args.exclude, args.limit)
    if not picked:
        print(f"ERROR: no assets matched (filters={args.filter or ['*']} "
              f"excludes={args.exclude}) in {tgl_dir} "
              f"({len(candidates)} candidates)", file=sys.stderr)
        return 2

    name = args.name or args.recipe
    out_dir = Path(args.out_dir) if args.out_dir else \
        TOOL_DIR / "out" / f"sweep_{name}"
    manifest = build_manifest(picked, out_dir, recipe=args.recipe,
                              params=params, out_kind=args.out_kind,
                              asset_class=args.asset_class,
                              override_class=args.override_class)

    errors = bd.validate_manifest(manifest, out_dir)
    if errors:
        print("GENERATED MANIFEST INVALID (sweep bug or bad --param):",
              file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 2

    manifest_text = json.dumps(manifest, indent=2) + "\n"
    if args.manifest_out:
        Path(args.manifest_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.manifest_out).write_text(manifest_text, encoding="utf-8")
        print(f"manifest: {args.manifest_out} ({len(picked)} job(s)) - dry mode, "
              "not running")
        return 0

    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "sweep_manifest.json"
    manifest_path.write_text(manifest_text, encoding="utf-8")

    shards = shard_jobs(manifest["jobs"], args.jobs)
    print(f"SWEEP {name} - recipe={args.recipe} out_kind={args.out_kind}")
    print(f"  srcdata:  {tgl_dir} ({len(candidates)} .ase with matching .ini)")
    print(f"  matched:  {len(picked)} asset(s) "
          f"(filter={args.filter or ['*']} exclude={args.exclude} "
          f"limit={args.limit})")
    print(f"  shards:   {len(shards)} (max {args.jobs} concurrent Blender)")
    print(f"  out dir:  {out_dir}")
    print(f"  manifest: {manifest_path}")
    print(f"  assets:   {', '.join(s for s, _, _ in picked)}")
    if not args.run:
        print("plan only - pass --run to execute")
        return 0

    ctx = {"base_dir": out_dir, "work_dir": out_dir / "_work",
           "deploy_tgl_dir": args.deploy_tgl_dir}
    try:
        ctx["blender_exe"] = find_blender_exe(args.blender)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"  blender:  {ctx['blender_exe']}")

    t0 = time.time()
    results = run_sweep(manifest, ctx, args.jobs)
    wall = time.time() - t0

    ok, fail = write_report(results, out_dir, wall_seconds=wall)
    print(f"\nSWEEP DONE - {ok} ok, {fail} failed, wall {wall:.1f}s")
    print(f"report: {out_dir / 'report.md'}")
    print(f"        {out_dir / 'report.csv'}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())

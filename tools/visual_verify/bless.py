#!/usr/bin/env python3
"""VISUAL-VERIFY-PIPELINE-1 -- bless a capture run's region crops as a golden set.

A golden is the reference a future "verify this build" run diffs against. We store
GOLDENS AS PER-REGION CROPS (not full 4K frames): the committed regions are small
strips, so a whole bookmark set is a few hundred KB of PNG -- cheap to keep in the
repo under tests/visual/goldens/<name>/. (Full frames are ~5-10 MB each; if you
ever need frame-level goldens, keep them in an external dir and record the path in
golden.json's `frames_dir` -- see docs/visual-verify.md.)

This tool:
  1. Re-emits every region crop from the run's captured PNGs (via verify's crop
     path) into tests/visual/goldens/<name>/<bookmark>/<region>.png.
  2. Writes golden.json: the identity block (install, exe, frame, per-crop sha256,
     source capture manifest) so the golden is self-describing and auditable.

Bless is a deliberate, reviewable act -- the crops + golden.json are what a human
commits. Nothing here pushes.

-----------------------------------------------------------------------------
VERIFY-DEBT-1 -- three refusals, all of them CODE (they used to be prose)
-----------------------------------------------------------------------------
A golden that cannot fail is worse than no golden: it consumes a slot in the
"N regions covered" count while proving nothing. Three classes of un-failable or
untrustworthy golden are now refused by this tool rather than by reviewer
vigilance.

R1 -- THREE-RUN ALL-PAIRS RULE (was prose only; nothing enforced it).
  Blessing requires N >= MIN_RUNS (3) independent capture runs of the same
  poses, and grades ALL C(N,2) pairs of every region against that region's own
  verify thresholds. Two runs are NOT enough: a region can agree perfectly on
  one pair and diverge badly on another (this rule has caught exactly that --
  0.0000% on one pair, 9.07% on another, in the same region). Fewer than three
  runs => refuse to mint the golden at all.

R2 -- UNSTABLE REGION. If a region's WORST pair already exceeds the thresholds
  verify.py will later grade candidates against, then its own run-to-run noise
  swamps the signal: every future verify is a coin flip. Such a region is
  refused (no crop written, recorded in golden.json's `refused` list with its
  measured floor) instead of being blessed into a flaky gate.

R3 -- DEGENERATE CROP. An all-black / all-white / near-flat crop can never fail
  a diff, so blessing it manufactures fake coverage. Refused. This is what
  caught mc2_10_saved_204907/distant_ridge_base: 300x100, 167 bytes, exactly ONE
  unique colour, byte-identical across three independent runs while its sibling
  regions in the same frames carried 36k+ colours -- i.e. not a silent capture
  failure but a genuinely void rect (night sky above the ridge it claimed to
  frame), sitting in the set marked severity=critical.

Provenance is TOOL-EMITTED, never hand-added (PROVENANCE-EMIT-1). blessed_commit,
gate_state and capture_env used to be written by hand into golden.json after the
fact -- which means the audit trail was exactly as trustworthy as the human who
typed it. All three are now derived here: the commit from git, the resolution
from each frame's own IHDR, the env from the capture manifests.

Usage:
  py -3 tools/visual_verify/bless.py --run <run1> --run <run2> --run <run3> \
      --to tests/visual/goldens/v0.5.1-baseline
"""
from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import math
import struct
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

_THIS = Path(__file__).resolve()
ROOT = _THIS.parents[2]
sys.path.insert(0, str(_THIS.parents[1] / "visual"))
sys.path.insert(0, str(_THIS.parent))
import regions as regionlib  # noqa: E402
from verify import diff_crops, grade, DEFAULT_THRESHOLDS  # noqa: E402
from png_io import decode_png  # noqa: E402

DEFAULT_GOLDENS = ROOT / "tests" / "visual" / "goldens"

# R1: minimum independent capture runs required to bless. Three is the smallest
# N for which "all pairs agree" is a stronger statement than "one pair agrees".
MIN_RUNS = 3

# R3: degeneracy floors. Both are deliberately far below anything a real render
# produces -- they are a trap for void/failed crops, not a quality bar. Verified
# against all 28 members of baseline-5ef261f4: exactly ONE was refused (the known
# void rect), and the tightest surviving real crop measured 421 unique colours
# (mc2_10_saved_204611/night_base_lighting) and stddev 3.4747 -- i.e. 52x margin
# on MIN_UNIQUE_COLORS and 6.9x on MIN_LUMA_STDDEV. Note the tightest crops are
# NIGHT poses, the exact case most at risk of a false positive: the floors clear
# them comfortably.
MIN_UNIQUE_COLORS = 8
MIN_LUMA_STDDEV = 0.5


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _git(root: Path, *args: str) -> "str | None":
    try:
        out = subprocess.run(["git", "-C", str(root), *args],
                             capture_output=True, text=True, timeout=30)
        if out.returncode != 0:
            return None
        return out.stdout.strip()
    except Exception:
        return None


def crop_stats(crop_png: bytes) -> dict:
    """Cheap degeneracy statistics over a crop: unique colours + luma stddev."""
    w, h, ch, px = decode_png(crop_png)
    colors = set()
    n = 0
    s = 0.0
    s2 = 0.0
    for i in range(0, len(px), ch):
        r, g, b = px[i], px[i + 1], px[i + 2]
        colors.add((r, g, b))
        y = 0.299 * r + 0.587 * g + 0.114 * b
        n += 1
        s += y
        s2 += y * y
    mean = s / n if n else 0.0
    var = max(0.0, (s2 / n) - mean * mean) if n else 0.0
    return {
        "unique_colors": len(colors),
        "luma_mean": round(mean, 4),
        "luma_stddev": round(math.sqrt(var), 4),
        "pixels": n,
        "bytes": len(crop_png),
    }


def degeneracy_reason(stats: dict) -> "str | None":
    """R3: return a refusal reason if this crop can never meaningfully fail."""
    if stats["unique_colors"] < MIN_UNIQUE_COLORS:
        return (f"degenerate crop: only {stats['unique_colors']} unique colour(s) "
                f"(floor {MIN_UNIQUE_COLORS}) -- luma mean {stats['luma_mean']}, "
                f"{stats['bytes']} bytes. A flat crop cannot fail a diff, so "
                f"blessing it manufactures coverage. Re-author the region rect to "
                f"frame real geometry, or drop the region.")
    if stats["luma_stddev"] < MIN_LUMA_STDDEV:
        return (f"degenerate crop: luma stddev {stats['luma_stddev']} below floor "
                f"{MIN_LUMA_STDDEV} (near-flat; {stats['unique_colors']} colours, "
                f"mean {stats['luma_mean']}). Cannot meaningfully fail a diff.")
    return None


def _load_runs(run_dirs: "list[Path]") -> "list[dict]":
    runs = []
    for rd in run_dirs:
        rd = Path(rd).resolve()
        cap_path = rd / "capture_run.json"
        if not cap_path.exists():
            raise SystemExit(f"[vv-bless] no capture_run.json in {rd}")
        cap = json.loads(cap_path.read_text(encoding="utf-8"))
        runs.append({"dir": rd, "path": cap_path, "cap": cap})
    return runs


def bless(run_dirs: "list[Path]", to_dir: Path, min_runs: int = MIN_RUNS) -> dict:
    run_dirs = [Path(r) for r in run_dirs]

    # ---- R1: three-run all-pairs rule, enforced before anything is written ----
    if len(set(str(Path(r).resolve()) for r in run_dirs)) != len(run_dirs):
        raise SystemExit("[vv-bless] refusing to bless: the same run directory was "
                         "passed more than once. All-pairs grading over duplicated "
                         "runs is self-comparison, not reproducibility.")
    if len(run_dirs) < min_runs:
        raise SystemExit(
            f"[vv-bless] refusing to bless: {len(run_dirs)} capture run(s) supplied, "
            f"minimum is {min_runs} (THREE-RUN-ALL-PAIRS-1). Two runs cannot "
            f"distinguish 'reproducible' from 'happened to agree once' -- this rule "
            f"has caught a region that scored 0.0000% on one pair and 9.07% on "
            f"another. Capture {min_runs - len(run_dirs)} more run(s) and re-bless.")

    runs = _load_runs(run_dirs)
    to_dir = Path(to_dir).resolve()
    to_dir.mkdir(parents=True, exist_ok=True)

    manifests = regionlib.load_all_manifests()

    # Index each run's poses by bookmark.
    per_run_poses = []
    for r in runs:
        per_run_poses.append({p["bookmark"]: p for p in r["cap"].get("poses", [])})

    members = []
    refused = []
    total_bytes = 0
    _frame_res: set = set()

    for bm in sorted(per_run_poses[0]):
        doc = manifests.get(bm)
        if doc is None:
            continue
        # every run must carry this pose, else it is not N-run reproducible
        pngs = []
        missing = False
        for i, poses in enumerate(per_run_poses):
            p = poses.get(bm)
            png_path = p.get("primary_png") if p else None
            if not png_path or not Path(png_path).exists():
                missing = True
                break
            pngs.append(Path(png_path).read_bytes())
        if missing:
            refused.append({"bookmark": bm, "region": "*",
                            "reason": f"pose absent or frame missing in at least one "
                                      f"of the {len(runs)} runs; cannot grade all-pairs"})
            continue

        for b in pngs:
            try:
                _w, _h = struct.unpack(">II", b[16:24])
                _frame_res.add(f"{int(_w)}x{int(_h)}")
            except Exception:
                pass

        bm_dir = to_dir / bm
        for reg in doc["regions"]:
            rid = reg["id"]
            thr = reg.get("thresholds") or DEFAULT_THRESHOLDS
            try:
                crops = [regionlib.crop_region_png(b, reg["rect"]) for b in pngs]
            except Exception as e:
                refused.append({"bookmark": bm, "region": rid,
                                "reason": f"crop failed: {e}"})
                continue
            cw, ch, crop0 = crops[0]

            # ---- R3: degenerate crop ----
            stats = crop_stats(crop0)
            dr = degeneracy_reason(stats)
            if dr is not None:
                refused.append({"bookmark": bm, "region": rid, "reason": dr,
                                "severity": reg.get("severity"), "stats": stats,
                                "rule": "DEGENERATE-CROP-REFUSAL-1"})
                continue

            # ---- R1/R2: all-pairs reproducibility ----
            repro = {}
            worst = 0.0
            worst_pair = None
            unstable = []
            for i, j in itertools.combinations(range(len(crops)), 2):
                m = diff_crops(crops[i][2], crops[j][2])
                key = f"r{i + 1}_vs_r{j + 1}_changed_pct"
                repro[key] = m["changed_pct"]
                if m["changed_pct"] > worst:
                    worst = m["changed_pct"]
                    worst_pair = f"r{i + 1}/r{j + 1}"
                verdict, reason = grade(m, thr)
                if verdict != "PASS":
                    unstable.append(f"r{i + 1}/r{j + 1}: {reason}")
            repro["worst_changed_pct"] = worst
            repro["worst_pair"] = worst_pair
            repro["pairs_graded"] = len(list(itertools.combinations(range(len(crops)), 2)))
            repro["runs"] = len(crops)

            if unstable:
                refused.append({
                    "bookmark": bm, "region": rid,
                    "severity": reg.get("severity"),
                    "reason": (f"unstable: run-to-run noise already exceeds the "
                               f"thresholds verify.py grades candidates against, so "
                               f"every future verify would be a coin flip. "
                               + "; ".join(unstable)),
                    "reproducibility": repro,
                    "rule": "THREE-RUN-ALL-PAIRS-1",
                })
                continue

            bm_dir.mkdir(parents=True, exist_ok=True)
            out = bm_dir / f"{rid}.png"
            out.write_bytes(crop0)
            total_bytes += len(crop0)
            members.append({
                "bookmark": bm, "region": rid,
                "path": str(out.relative_to(to_dir)).replace("\\", "/"),
                "size": f"{cw}x{ch}",
                "sha256": hashlib.sha256(crop0).hexdigest(),
                "severity": reg.get("severity"),
                "thresholds": reg.get("thresholds"),
                "check": reg.get("check"),
                "reproducibility": repro,
                "crop_stats": stats,
            })

    cap0 = runs[0]["cap"]
    golden = {
        "kind": "visual-verify-golden",
        "schema_v": 2,
        "golden_id": to_dir.name,
        "blessed_at": _now_iso(),
        "source_capture": str(runs[0]["path"]),
        "source_captures": [str(r["path"]) for r in runs],
        "install": cap0.get("install"),
        "exe": cap0.get("exe"),
        "exe_mtime": cap0.get("exe_mtime"),
        "frame": cap0.get("frame"),
        "deterministic_clock": cap0.get("deterministic_clock"),
        "n_regions": len(members),
        "n_refused": len(refused),
        "total_crop_bytes": total_bytes,
        "refused": refused,
        "members": members,
    }

    # ---- PROVENANCE-EMIT-1: derived here, never hand-added ----
    # A golden that cannot state its own reference resolution cannot be
    # checked. Refuse to mint one rather than mint an untrustworthy one.
    if len(_frame_res) != 1:
        raise SystemExit(
            f"[vv-bless] refusing to bless: source frames have "
            f"{'inconsistent' if _frame_res else 'unreadable'} resolutions "
            f"{sorted(_frame_res) or '[]'}. A golden must carry exactly one "
            f"capture_env.resolution (CAPTURE-RESOLUTION-ASSERT-1).")

    head = _git(ROOT, "rev-parse", "HEAD")
    dirty = _git(ROOT, "status", "--porcelain")
    golden["blessed_commit"] = head
    golden["blessed_tree_dirty"] = bool(dirty) if dirty is not None else None
    golden["provenance_emitted_by"] = "tools/visual_verify/bless.py (PROVENANCE-EMIT-1)"

    env_keys: set = set()
    for r in runs:
        for k in (r["cap"].get("effective_mc2_env") or []):
            env_keys.add(k)
    golden["capture_env"] = {
        "resolution": next(iter(_frame_res)),
        "runs": len(runs),
        "min_runs_rule": min_runs,
        "trigger_frame": cap0.get("frame"),
        "settle": cap0.get("settle"),
        "deterministic_clock": cap0.get("deterministic_clock"),
        "effective_mc2_env": sorted(env_keys),
    }
    golden["gate_state"] = {
        "source": "capture manifests",
        "effective_mc2_env": sorted(env_keys),
    }

    (to_dir / "golden.json").write_text(json.dumps(golden, indent=2) + "\n",
                                        encoding="utf-8")
    return golden


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--run", required=True, action="append", dest="runs",
                    help=f"capture dir to bless; pass at least {MIN_RUNS} times "
                         f"(THREE-RUN-ALL-PAIRS-1)")
    ap.add_argument("--to", required=True,
                    help="golden dir (e.g. tests/visual/goldens/v0.5.1-baseline)")
    ap.add_argument("--min-runs", type=int, default=MIN_RUNS,
                    help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    golden = bless([Path(r) for r in args.runs], Path(args.to), args.min_runs)
    kb = golden["total_crop_bytes"] / 1024.0
    print(f"[vv-bless] blessed {golden['n_regions']} region crops "
          f"({kb:.1f} KB) from {golden['capture_env']['runs']} runs "
          f"-> {args.to}")
    for r in golden["refused"]:
        print(f"[vv-bless] REFUSED {r['bookmark']}/{r['region']}: {r['reason']}")
    print(f"[vv-bless] golden.json: {Path(args.to).resolve() / 'golden.json'}")
    print("[vv-bless] BLESS is complete on disk; commit the golden dir to make it "
          "the reference. Provenance (blessed_commit / capture_env / gate_state) is "
          "tool-emitted -- do not hand-edit it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

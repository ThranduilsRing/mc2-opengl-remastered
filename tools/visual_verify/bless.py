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

Provenance is TOOL-EMITTED, never hand-added (PROVENANCE-EMIT-1). gate_state and
capture_env used to be written by hand into golden.json after the fact -- which
means the audit trail was exactly as trustworthy as the human who typed it. Both
are now derived here: the resolution from each frame's own IHDR, the env from
the capture manifests.

-----------------------------------------------------------------------------
GOLDEN-PROVENANCE-TRUTH-1 -- R4/R5/R6, and what `blessed_commit` was
-----------------------------------------------------------------------------
Until this change, the most authoritative artifact class in the repo recorded
its binary by PATH + MTIME and stamped `blessed_commit` from `git rev-parse
HEAD` in the BLESSER'S WORKTREE. That is the source-HEAD-as-provenance defect
CAPTURE-PROVENANCE-TRUTH-1 removed from capture manifests, laundered straight
back into the goldens -- and the ratchet could not see it, because `golden.json`
was in neither the checker's filename list nor its scan roots.

It was not hypothetical. Measured 2026-07-30 over the three committed goldens:
`v0.5.1-baseline` and `v0.5.2-baseline-PROPOSED` name the SAME exe path with two
different mtimes; the file there today has the second mtime, so the binary that
minted the first is overwritten and gone. `baseline-5ef261f4`'s hand-added
identity block claims exe_sha256 99a12c41...; the file at its path today is
4b195092..., a different lane's build. NONE of the three can prove which binary
minted it. `blessed_commit` was null in all three, and would have been a
different lane's HEAD if it had not been.

  R4 -- BINARY IDENTIFIED. Every source run's capture manifest must name its
    binary by CONTENT (identity.exe.sha256) and carry that binary's own embedded
    build fingerprint. A legacy path+mtime capture is refused: the crops it
    produced cannot be attributed, so a golden minted from them cannot be
    either.

  R5 -- ONE BINARY. All runs must name the SAME binary sha256, and the file on
    disk (when still present) must still be those bytes. Three runs of three
    different builds have an all-pairs "reproducibility" number that measures
    nothing, and R1 would have blessed it.

  R6 -- DISTINCT FRAMES. Two runs must not name the same primary PNG file. R1
    rejects duplicated run DIRECTORIES; nothing stopped three manifests pointing
    at one set of images, which is the same self-comparison one level down.

The golden now carries the unified identity block (scripts/manifest_schema.py):
`identity.binary_provenance.commit` is the commit compiled INTO the binary, read
out of its bytes. The blesser's worktree HEAD is still recorded -- it is useful
for finding the lane afterwards -- but under the name
`source_worktree_head_at_bless`, carrying `describes_binary: false` IN the JSON,
so a reader with no access to this docstring cannot mistake it for provenance.

-----------------------------------------------------------------------------
PROVENANCE-FORGERY-CLOSE-1 -- this file was still writing the defect it named
-----------------------------------------------------------------------------
Deleting `blessed_commit` moved the lie; it did not remove it. Until this
change, three lines below the paragraph above, bless.py called
`ms.identity_block(repo_root=str(ROOT))` -- and `identity_block` fills
`source_worktree_head_at_capture`, its legacy alias `git`, and
`binary_provenance` from THAT root. So the BLESSER'S HEAD landed in two of the
exact spellings scripts/provenance_gate.py lists under "none of them describe
the binary", one of them named `..._at_capture` while describing neither the
capture's worktree nor the capture's moment.

The consequence was not cosmetic: `binary_provenance` reconciles the binary's
compiled-in commit against that head, so every HONEST cross-lane golden -- the
normal case, since the lane that captures is routinely not the lane that
blesses -- was stamped `SOURCE-HEAD-MISMATCH`. A verdict that fires on the
normal case is noise, and noise is what gets ignored when it is real.

The checker added in the same commit could not see it, because it looks only for
a TOP-LEVEL `blessed_commit`: the field that was deleted, not the mechanism that
produced it.

Now: `identity_block` is called with `repo_root=None` and `exe_path=None`, and
every field claiming to describe the capture is carried forward from the CAPTURE
manifest -- the exe block, the source head, and the reconciliation against it.
The blesser's HEAD appears exactly once, under `source_worktree_head_at_bless`,
with `describes_binary: false`.

The golden also now emits `identity.binary_corroboration.witnesses`: the source
capture manifests, each pinned by its OWN sha256. provenance_gate.establish()
requires a witness from OUTSIDE the artifact before it will grade -- self-
consistency is a file agreeing with itself, which is exactly what a forger
supplies.

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
sys.path.insert(0, str(ROOT / "scripts"))
import regions as regionlib  # noqa: E402
from verify import diff_crops, grade, DEFAULT_THRESHOLDS  # noqa: E402
from png_io import decode_png  # noqa: E402
import manifest_schema as ms  # noqa: E402  (unified identity + binary provenance)
import provenance_gate as pg  # noqa: E402  (GOLDEN-PROVENANCE-TRUTH-1)
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from smoke_lib import render_options_pin  # noqa: E402  (the render options are an input of the picture)
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

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

# A golden that names its backend (--backend) is the reference for that backend only: the VK set replaces the GL set before GL is deleted
# (tools/visual_verify/vk_golden_plan.md). Each backend lists the environment its runs must have declared, beside the animation freeze that
# every named-backend golden needs (an unfrozen night pose is the run-to-run noise that refused 13 regions of the GL 1080p set).
BACKEND_DECLARED_ENV = {
    "gl": {},
    "vulkan": {"MC2_RENDER_BACKEND": "vulkan", "MC2_VK_SHARED_GBUFFER": "1"},
}
FREEZE_ENV = {"MC2_SMOKE_FREEZE_ANIM": "1"}


def require_backend(caps: "list[dict]", backend: str) -> None:
    """SystemExit unless every capture manifest in `caps` asked for `backend`, was drawn by it alone and declared its environment."""
    if backend not in BACKEND_DECLARED_ENV:
        raise SystemExit(f"[vv-bless] unknown backend {backend!r} (known: {', '.join(sorted(BACKEND_DECLARED_ENV))})")
    want_env = dict(FREEZE_ENV, **BACKEND_DECLARED_ENV[backend])
    for i, cap in enumerate(caps, start=1):
        rec = cap.get("backend") or {}
        drew = rec.get("drew")
        drew = [drew] if isinstance(drew, str) else list(drew or [])
        if rec.get("requested") != backend or drew != [backend]:
            raise SystemExit(f"[vv-bless] refusing to bless a {backend} golden: run {i} asked for {rec.get('requested')!r} and was drawn by "
                             f"{drew or 'an unrecorded backend'}; only frames drawn by {backend} alone are evidence for it.")
        declared = cap.get("declared_env") or {}
        missing = [f"{k}={v}" for k, v in want_env.items() if str(declared.get(k)) != v]
        if missing:
            raise SystemExit(f"[vv-bless] refusing to bless a {backend} golden: run {i} did not declare {', '.join(missing)}; "
                             f"recapture with capture.py --env for each.")


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _sha256_path(p: Path) -> "str | None":
    """Digest of a whole file. Used to PIN each corroborating witness by its own
    content, so a witness edited after the bless stops corroborating."""
    try:
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:  # noqa: BLE001
        return None


def _git(root: Path, *args: str) -> "str | None":
    try:
        out = subprocess.run(["git", "-C", str(root), *args],
                             capture_output=True, text=True, timeout=30,
                             **nofocus.hidden_kwargs())
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


def bless(run_dirs: "list[Path]", to_dir: Path, min_runs: int = MIN_RUNS, named_backend: "str | None" = None) -> dict:
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

    # ---- R4/R5: the source runs must name ONE binary, by content ------------
    # Nothing downstream can repair an unattributable input, so this is checked
    # BEFORE a single crop is written. `establish` accepts only a tool-emitted
    # exe digest plus that binary's own embedded build stamp, read from those
    # same bytes -- never a path+mtime, never a hand-added claim.
    ests = [pg.establish(r["cap"], f"run{i + 1} ({r['dir'].name})")
            for i, r in enumerate(runs)]
    try:
        pg.refuse_unless_established(
            ests,
            decision=("mint a golden -- the reference every later release is "
                      "graded against"),
            remedy=("recapture with the current tools/visual_verify/capture.py, "
                    "which emits identity.exe.sha256 + the binary's embedded "
                    "build fingerprint (manifest_schema.identity_block). A "
                    "capture that only names its exe by path and mtime cannot "
                    "be attributed to any binary, so neither can a golden "
                    "minted from it."))
    except pg.ProvenanceUnestablished as e:
        raise SystemExit(f"[vv-bless] refusing to bless: {e}")

    ok_same, detail = pg.same_binary(ests)
    if not ok_same:
        raise SystemExit(
            f"[vv-bless] refusing to bless: the {len(runs)} capture runs did "
            f"NOT measure the same binary ({detail}). The three-run all-pairs "
            f"rule (R1) grades run-to-run reproducibility; across different "
            f"builds that number measures a code change, not noise, and a "
            f"golden minted from it would carry a reproducibility claim it "
            f"never earned.")

    # ---- the render options are an input of every crop: the runs must record them and agree ----
    # A golden minted from runs with unrecorded or differing options could not say what picture it is the reference for, and verify.py could not
    # tell a real regression from a different options.cfg.
    run_options = [render_options_pin.values_of({"values": r["cap"].get("render_options")}) for r in runs]
    if not all(run_options):
        raise SystemExit(
            "[vv-bless] refusing to bless: a capture run carries no render-option record (capture_run.json `render_options`). The 17 options.cfg "
            "render options are an input of the picture; recapture with the current tools/visual_verify/capture.py, which records them.")
    for i, opts in enumerate(run_options[1:], start=2):
        diffs = render_options_pin.differences(run_options[0], opts)
        if diffs:
            raise SystemExit(f"[vv-bless] refusing to bless: run 1 and run {i} ran with different render options ({'; '.join(diffs)}); "
                             f"three runs of one configuration are what the reproducibility rule grades.")
    # ---- and the backend that drew them must be the one they asked for ----
    for i, r in enumerate(runs, start=1):
        backend = r["cap"].get("backend") or {}
        drew = backend.get("drew")
        drew = [drew] if isinstance(drew, str) else list(drew or [])
        if backend.get("requested") and drew and drew != [backend["requested"]]:
            raise SystemExit(f"[vv-bless] refusing to bless: run {i} asked for the {backend['requested']} backend and {drew} drew it; "
                             f"its frames are not evidence for the backend a golden names.")
    if named_backend is not None:
        require_backend([r["cap"] for r in runs], named_backend)

    to_dir = Path(to_dir).resolve()
    to_dir.mkdir(parents=True, exist_ok=True)

    manifests = regionlib.load_all_manifests()

    # Index each run's poses by bookmark.
    per_run_poses = []
    for r in runs:
        per_run_poses.append({p["bookmark"]: p for p in r["cap"].get("poses", [])})

    members = []
    refused = []
    subjects = {}  # (bookmark, region) -> region manifest `check` text
    total_bytes = 0
    _frame_res: set = set()

    for bm in sorted(per_run_poses[0]):
        doc = manifests.get(bm)
        if doc is None:
            continue
        # every run must carry this pose, else it is not N-run reproducible
        pngs = []
        png_paths: list = []
        missing = False
        for i, poses in enumerate(per_run_poses):
            p = poses.get(bm)
            png_path = p.get("primary_png") if p else None
            if not png_path or not Path(png_path).exists():
                missing = True
                break
            png_paths.append(Path(png_path).resolve())
            pngs.append(Path(png_path).read_bytes())
        # ---- R6: DISTINCT FRAMES ------------------------------------------
        # R1 rejects the same run DIRECTORY passed twice. Nothing stopped N
        # manifests naming ONE set of images, which is the identical
        # self-comparison a level down: every pair would score 0.0000% and the
        # golden would record an all-pairs reproducibility it never measured.
        if not missing and len(set(str(p) for p in png_paths)) != len(png_paths):
            dupes = sorted({str(p) for p in png_paths
                            if png_paths.count(p) > 1})
            raise SystemExit(
                f"[vv-bless] refusing to bless: bookmark {bm!r} names the SAME "
                f"primary PNG in more than one run ({dupes}). All-pairs "
                f"grading over one image is self-comparison: every pair scores "
                f"0.0000% and the golden would claim a reproducibility nobody "
                f"measured.")
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
            # check-golden-coverage requires every refused hole to say what it
            # protected; the region manifest's `check` text is that statement.
            subjects[(bm, rid)] = reg.get("check") or rid
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
        "schema_v": 3,
        "golden_id": to_dir.name,
        "blessed_at": _now_iso(),
        "source_capture": str(runs[0]["path"]),
        "source_captures": [str(r["path"]) for r in runs],
        "install": cap0.get("install"),
        "exe": cap0.get("exe"),
        "exe_mtime": cap0.get("exe_mtime"),
        # GOLDEN-PROVENANCE-TRUTH-1: `exe` above is a path and `exe_mtime` an
        # mtime -- kept only for back-compat with schema_v<=2 readers. NEITHER
        # is an identity. The binary is identified by content here and, in full,
        # under `identity`.
        "exe_sha256": ests[0].get("exe_sha256"),
        "frame": cap0.get("frame"),
        "deterministic_clock": cap0.get("deterministic_clock"),
        "n_regions": len(members),
        "n_refused": len(refused),
        "total_crop_bytes": total_bytes,
        "refused": [dict(h, subject_protected=h.get("subject_protected")
                         or subjects.get((h["bookmark"], h["region"]))
                         or "whole pose " + h["bookmark"])
                    for h in refused],
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

    # ---- GOLDEN-PROVENANCE-TRUTH-1: WHAT WAS MEASURED, then what the blesser
    # happened to be sitting on. The two are named so they cannot be confused.
    #
    # `blessed_commit` USED TO LIVE HERE and was `git rev-parse HEAD` in the
    # BLESSER's worktree. It described the blesser's lane, not the binary that
    # rendered a single one of these crops, and it sat beside a `dirty` flag
    # that made the pair read as authoritative. It is deleted, not renamed in
    # place: a reader looking for it must fail to find it and go read
    # `identity.binary_provenance` instead.
    # PROVENANCE-FORGERY-CLOSE-1. `repo_root` and `exe_path` are deliberately
    # NOT passed. Passing them made `identity_block` fill
    # `source_worktree_head_at_capture`, `git` and `binary_provenance` from the
    # BLESSER'S worktree and a LIVE re-read of today's file -- i.e. the exact
    # source-HEAD-as-provenance defect this module's own docstring lists under
    # "none of them describe the binary", re-introduced two fields below the
    # paragraph that forbids it. `identity_block(repo_root=None)` returns an
    # all-null head carrying the honest describes_binary=false labels; every
    # field that claims to describe the CAPTURE is then filled from the capture.
    ident = ms.identity_block(
        generator="visual_verify_bless",
        exe_path=None,
        repo_root=None,
        deploy_target=cap0.get("install"),
    )
    # The exe at that path may have been REBUILT between capture and bless (it
    # is, right now, for two of the three committed goldens). The binary the
    # RUNS measured is the authority, so the runs' recorded exe block is carried
    # verbatim. Re-reading the path live would describe whatever binary sits
    # there NOW, not the one that rendered these crops -- that substitution is
    # the defect, not a fallback. So its absence is fatal.
    cap_ident = (runs[0]["cap"].get("identity") or {})
    src_exe_blk = cap_ident.get("exe")
    if not isinstance(src_exe_blk, dict):
        raise SystemExit(
            "[vv-bless] refusing to bless: the source capture established its "
            "binary but carries no identity.exe block to copy forward. "
            "Recording the live re-read of that path instead would describe "
            "whatever binary sits there NOW, not the one that rendered these "
            "crops -- that substitution is the defect, not a fallback.")
    ident["exe"] = src_exe_blk

    # The source head that belongs in a field named `..._at_capture` is the one
    # the CAPTURE recorded, not the blesser's. Carried forward verbatim, exactly
    # like the exe block, and for the same reason.
    cap_head = cap_ident.get("source_worktree_head_at_capture") or cap_ident.get("git")
    if not isinstance(cap_head, dict):
        cap_head = {
            "commit": None, "commit_short": None, "branch": None, "dirty": None,
            "describe": None, "worktree": None,
            "describes": "source_worktree_head_at_capture",
            "describes_binary": False,
            "unavailable_reason": (
                "the source capture manifest recorded no worktree head. It is "
                "left NULL rather than filled from the blessing worktree: a "
                "wrong commit under a right-sounding name is worse than none."),
            "note": ms.SOURCE_HEAD_NOTE,
        }
    ident["source_worktree_head_at_capture"] = cap_head
    ident["git"] = cap_head  # legacy alias; must stay byte-identical to the above

    # Reconciled against the CAPTURE's head. Reconciling against the BLESSER's
    # head stamped `SOURCE-HEAD-MISMATCH` on every honest cross-lane golden --
    # blessing from a different worktree than the one that built the exe is the
    # normal case here, and a verdict that fires on the normal case is noise.
    ident["binary_provenance"] = ms.binary_provenance(src_exe_blk, cap_head)
    ident["binary_identity_established"] = {
        "level": ests[0]["level"],
        "checked_by": "scripts/provenance_gate.py (GOLDEN-PROVENANCE-TRUTH-1)",
        "runs_agree_on_binary": True,
        "detail": ests[0]["why"],
    }

    # ---- PROVENANCE-FORGERY-CLOSE-1: name the independent witnesses ---------
    # `establish()` used to grade a record OK on the strength of the record
    # agreeing with itself. It now requires something OUTSIDE the file. The
    # source capture manifests ARE that something: separate files, emitted by a
    # separate tool at a separate time, that independently record the same exe
    # digest. Each is pinned by its OWN sha256, so editing one to agree stops it
    # counting. This is emitted, never hand-added (PROVENANCE-EMIT-1).
    witnesses = []
    for r in runs:
        wsha = _sha256_path(r["path"])
        if not wsha:
            continue
        witnesses.append({
            "path": str(r["path"]),
            "artifact_sha256": wsha,
            "records_exe_sha256": ((r["cap"].get("identity") or {})
                                   .get("exe") or {}).get("sha256"),
            "kind": "source-capture-manifest",
        })
    ident["binary_corroboration"] = {
        "emitted_by": "tools/visual_verify/bless.py (PROVENANCE-FORGERY-CLOSE-1)",
        "witnesses": witnesses,
        "note": ("Independent artifacts that record the SAME exe digest as "
                 "identity.exe.sha256. Consumed by provenance_gate.establish() "
                 "as W-ARTIFACT: each must still exist, still hash to "
                 "`artifact_sha256`, and still name that digest. They are a "
                 "convenience, not the only arm -- if these capture trees are "
                 "pruned, W-REPO-COMMIT still corroborates from the git object "
                 "database, which outlives every install."),
    }
    golden["identity"] = ident

    head = _git(ROOT, "rev-parse", "HEAD")
    dirty = _git(ROOT, "status", "--porcelain", "-uno")
    golden["source_worktree_head_at_bless"] = {
        "commit": head,
        "branch": _git(ROOT, "rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(dirty) if dirty is not None else None,
        "worktree": str(ROOT),
        "describes": "source_worktree_head_at_bless",
        "describes_binary": False,
        "dirty_describes": ms.SOURCE_DIRTY_NOTE,
        "note": ("SOURCE WORKTREE HEAD of whoever ran bless.py. It does NOT "
                 "describe the binary that rendered these crops -- the blessing "
                 "worktree and the worktree that compiled the exe are routinely "
                 "different lanes, and blessing can happen days after capture. "
                 "For the commit that was actually exercised, read "
                 "identity.binary_provenance.commit. This block exists only to "
                 "find the lane that made the blessing decision."),
    }
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
        # What the frames were drawn with: the 17 render options (verify.py refuses a candidate whose options differ unless --vary names them),
        # the declared environment (animation freeze, backend selector, clean-capture switches) and the backend that drew.
        "render_options": run_options[0],
        "declared_env": cap0.get("declared_env") or {},
        "backend": cap0.get("backend") or {},
    }
    # The backend this golden is the reference for: the named one, else what run 1 asked for (a pre-flag golden records only capture_env).
    golden["backend"] = named_backend or (cap0.get("backend") or {}).get("requested")
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
    ap.add_argument("--backend", choices=sorted(BACKEND_DECLARED_ENV), default=None,
                    help="the backend this golden is the reference for: every run must have asked for it, been drawn by it alone and "
                         "declared MC2_SMOKE_FREEZE_ANIM=1 plus the backend's own env (vulkan: the umbrella pair); recorded as golden.json `backend`")
    args = ap.parse_args(argv)

    golden = bless([Path(r) for r in args.runs], Path(args.to), args.min_runs, named_backend=args.backend)
    kb = golden["total_crop_bytes"] / 1024.0
    print(f"[vv-bless] blessed {golden['n_regions']} region crops "
          f"({kb:.1f} KB) from {golden['capture_env']['runs']} runs "
          f"-> {args.to}")
    print(f"[vv-bless] backend: {golden['backend']}")
    for r in golden["refused"]:
        print(f"[vv-bless] REFUSED {r['bookmark']}/{r['region']}: {r['reason']}")
    bp = (golden.get("identity") or {}).get("binary_provenance") or {}
    print(f"[vv-bless] binary measured: commit={bp.get('commit')} "
          f"branch={bp.get('branch')} dirty={bp.get('dirty')} "
          f"sha256={str(golden.get('exe_sha256'))[:12]} "
          f"[{(golden['identity']['binary_identity_established'])['level']}]")
    print(f"[vv-bless] blesser's worktree HEAD (NOT provenance): "
          f"{str(golden['source_worktree_head_at_bless'].get('commit'))[:12]}")
    print(f"[vv-bless] golden.json: {Path(args.to).resolve() / 'golden.json'}")
    print("[vv-bless] BLESS is complete on disk; commit the golden dir to make it "
          "the reference. Provenance (identity.binary_provenance / capture_env / "
          "gate_state) is tool-emitted -- do not hand-edit it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

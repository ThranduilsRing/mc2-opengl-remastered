#!/usr/bin/env python3
"""VISUAL-VERIFY-PIPELINE-1 -- region crop + golden diff + verdict report.

Takes a capture-dir produced by capture.py and, for every pose that has a region
manifest (tools/visual_verify/regions/<bookmark>.json):

  1. Emits a full-resolution crop PNG for every named artifact region.
  2. If a golden crop for that region exists, computes per-region diff metrics
     (mean channel delta, changed-pixel %, max delta, and a block-structural
     delta) and grades PASS / FAIL against the per-region thresholds baked into
     the manifest. No golden  ->  NEW (and, in --baseline mode, the crop becomes
     the seed golden).
  3. Writes a verdict table (report.md), a machine report (report.json), and a
     crop contact-sheet dir a human or agent can Read for eyeball judgment.

This reuses the shared PNG codec (tools/visual/png_io) and the same
epsilon/mean-delta idea as tools/visual/visual_diff.py, but operates on the
committed *regions* rather than the whole frame -- so the automated grade lines
up with what the fleet actually cropped and complained about.

GOLDEN-PROVENANCE-TRUTH-1 (2026-07-30) -- this tool used to grade blind.
    It read crop PNGs out of the golden directory and never opened golden.json,
    so it could not have known -- and did not ask -- which binary minted the
    reference it was grading a release against. Measured over the three
    committed goldens: none of them can say. `verify_run` now refuses to grade
    unless BOTH the golden and the candidate capture name their own binary by
    content sha256 and carry that binary's embedded build fingerprint
    (scripts/provenance_gate.py). There is no override flag; the way out is to
    re-bless, or to drop --golden and get crops without a verdict.

Verdict math per region (candidate vs golden crop):
    FAIL if  mean_delta > t.mean_delta
         or  changed_pct > t.changed_pct
         or  struct_delta > t.struct        (when a 'struct' threshold is set)
    else PASS.
Missing golden -> NEW. Decode/size error -> SUSPECT.
Pose verdict = worst region verdict. Run exit = worst pose verdict.

Usage:
  # grade a candidate capture against blessed goldens:
  py -3 tools/visual_verify/verify.py --run <capture-dir> \
      --golden current          # = tests/visual/goldens/<render_resolution.CURRENT_GOLDEN>

  # baseline mode (no golden): just emit crops + a NEW-only report:
  py -3 tools/visual_verify/verify.py --run <capture-dir> --baseline
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

_THIS = Path(__file__).resolve()
sys.path.insert(0, str(_THIS.parents[1] / "visual"))
sys.path.insert(0, str(_THIS.parent))
sys.path.insert(0, str(_THIS.parents[2] / "scripts"))
from png_io import decode_png, encode_png  # noqa: E402
import regions as regionlib  # noqa: E402
import provenance_gate as pg  # noqa: E402  (GOLDEN-PROVENANCE-TRUTH-1)
from smoke_lib import render_options_pin  # noqa: E402  (the render options are an input of the picture)

EPSILON_LSB = 2  # per-channel LSB treated as dither noise (matches visual_diff.py)

# Default thresholds when a region omits some/all (kept lenient; determinism makes
# a true match ~0, so any real geometry/lighting change trips these).
DEFAULT_THRESHOLDS = {"mean_delta": 3.0, "changed_pct": 1.5, "struct": 12.0}

VERDICT_EXIT = {"PASS": 0, "NEW": 0, "FLIP": 1, "FAIL": 1, "SUSPECT": 2}


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _to_rgb(pix, channels, w, h):
    if channels == 3:
        return pix
    out = bytearray(w * h * 3)
    for p in range(w * h):
        out[p * 3] = pix[p * 4]
        out[p * 3 + 1] = pix[p * 4 + 1]
        out[p * 3 + 2] = pix[p * 4 + 2]
    return out


def _nearest_resize_rgb(rgb, w, h, tw, th):
    """Nearest-neighbour resize an RGB buffer WxH -> tw x th (only used when a
    candidate crop and its golden differ in size, e.g. a resolution change)."""
    if (w, h) == (tw, th):
        return rgb
    out = bytearray(tw * th * 3)
    for y in range(th):
        sy = min(h - 1, (y * h) // th)
        for x in range(tw):
            sx = min(w - 1, (x * w) // tw)
            s = (sy * w + sx) * 3
            d = (y * tw + x) * 3
            out[d] = rgb[s]
            out[d + 1] = rgb[s + 1]
            out[d + 2] = rgb[s + 2]
    return out


def _block_luma(rgb, w, h, grid=8):
    """Return a grid*grid list of average luma (0..255) -- a cheap structural
    fingerprint robust to small dither but sensitive to geometry/light moves."""
    cells = [0.0] * (grid * grid)
    counts = [0] * (grid * grid)
    for y in range(h):
        gy = min(grid - 1, (y * grid) // h)
        for x in range(w):
            gx = min(grid - 1, (x * grid) // w)
            i = (y * w + x) * 3
            luma = 0.299 * rgb[i] + 0.587 * rgb[i + 1] + 0.114 * rgb[i + 2]
            c = gy * grid + gx
            cells[c] += luma
            counts[c] += 1
    return [cells[i] / counts[i] if counts[i] else 0.0 for i in range(grid * grid)]


def diff_crops(golden_png: bytes, cand_png: bytes) -> dict:
    """Per-region metrics between two crop PNGs. Candidate is resized to golden
    dims if they differ (resolution change)."""
    gw, gh, gc, gp = decode_png(golden_png)
    cw, ch, cc, cp = decode_png(cand_png)
    g_rgb = _to_rgb(gp, gc, gw, gh)
    c_rgb = _to_rgb(cp, cc, cw, ch)
    if (gw, gh) != (cw, ch):
        c_rgb = _nearest_resize_rgb(c_rgb, cw, ch, gw, gh)
        cw, ch = gw, gh
    n = gw * gh
    changed = 0
    maxd = 0
    total = 0
    for p in range(n):
        i = p * 3
        d0 = abs(g_rgb[i] - c_rgb[i])
        d1 = abs(g_rgb[i + 1] - c_rgb[i + 1])
        d2 = abs(g_rgb[i + 2] - c_rgb[i + 2])
        m = d0 if d0 >= d1 else d1
        if d2 > m:
            m = d2
        if m > maxd:
            maxd = m
        total += d0 + d1 + d2
        if m > EPSILON_LSB:
            changed += 1
    gl = _block_luma(g_rgb, gw, gh)
    cl = _block_luma(c_rgb, gw, gh)
    struct = sum(abs(a - b) for a, b in zip(gl, cl)) / len(gl)
    return {
        "width": gw, "height": gh,
        "changed_px": changed,
        "changed_pct": round(100.0 * changed / n, 4) if n else 0.0,
        "max_delta": maxd,
        "mean_delta": round(total / (n * 3.0), 4) if n else 0.0,
        "struct_delta": round(struct, 4),
    }


def grade(metrics: dict, thresholds: dict) -> "tuple[str, str]":
    t = dict(DEFAULT_THRESHOLDS)
    t.update(thresholds or {})
    reasons = []
    if metrics["mean_delta"] > t["mean_delta"]:
        reasons.append(f"mean_delta {metrics['mean_delta']} > {t['mean_delta']}")
    if metrics["changed_pct"] > t["changed_pct"]:
        reasons.append(f"changed_pct {metrics['changed_pct']}% > {t['changed_pct']}%")
    if "struct" in t and metrics["struct_delta"] > t["struct"]:
        reasons.append(f"struct_delta {metrics['struct_delta']} > {t['struct']}")
    if reasons:
        return "FAIL", "; ".join(reasons)
    return "PASS", ""


def check_render_options(golden_dir: Path, cap: dict, vary: "list[str]") -> dict:
    """The golden's recorded render options against the candidate capture's. Raises SystemExit on a mismatch or a missing record (an options.cfg
    that differs is a different picture, not a regression); `vary` names the keys that differ on purpose. Returns both records for the report."""
    gj = Path(golden_dir) / "golden.json"
    try:
        golden_opts = ((json.loads(gj.read_text(encoding="utf-8")).get("capture_env") or {}).get("render_options"))
    except (OSError, ValueError):
        golden_opts = None
    cand_opts = cap.get("render_options")
    try:
        render_options_pin.require_same({"values": golden_opts}, {"values": cand_opts}, vary, "the golden", "the candidate capture")
    except render_options_pin.OptionsMismatch as e:
        raise SystemExit(f"[vv-verify] RENDER OPTIONS MISMATCH: {e}")
    return {"golden": golden_opts, "candidate": cand_opts, "vary": list(vary)}


def verify_run(run_dir: Path, golden_dir: "Path | None", baseline: bool, vary: "list[str] | None" = None) -> dict:
    run_dir = Path(run_dir).resolve()
    manifest_path = run_dir / "capture_run.json"
    if not manifest_path.exists():
        raise SystemExit(f"[vv-verify] no capture_run.json in {run_dir}")
    cap = json.loads(manifest_path.read_text(encoding="utf-8"))

    # GOLDEN-PROVENANCE-TRUTH-1 -- REFUSE TO GRADE AGAINST AN UNATTRIBUTABLE
    # GOLDEN. This is the decision point that mints a PASS/FAIL verdict against
    # a release, and until now it performed ZERO provenance checks: it read the
    # crop PNGs out of the golden dir and never opened golden.json at all. A
    # golden whose binary is unknown grades every candidate against an unknown,
    # and the resulting PASS looks exactly like a real one.
    #
    # Both sides are interrogated. The golden and the candidate are NOT required
    # to name the same binary -- comparing a new build to an old reference is
    # the entire point -- only to name THEIR OWN, by content, with that binary's
    # own embedded build stamp.
    #
    # Not gated in --baseline mode: nothing is graded there, every region is
    # NEW, and the seed crops become a golden only via bless.py, which runs the
    # same gate.
    if golden_dir is not None and not baseline:
        gj = Path(golden_dir) / "golden.json"
        if not gj.exists():
            raise SystemExit(
                f"[vv-verify] PROVENANCE UNESTABLISHED -- refusing to grade "
                f"against {golden_dir}: it has no golden.json, so it cannot "
                f"state which binary minted its crops. A directory of PNGs is "
                f"not a golden.")
        try:
            pg.refuse_unless_established(
                [pg.establish_path(gj, "golden"),
                 pg.establish(cap, "candidate capture")],
                decision=("grade a candidate against a golden -- this call "
                          "mints the PASS/FAIL a release is judged on"),
                remedy=(
                    "GOLDEN: re-bless it from captures that identify their "
                    "binary (tools/visual_verify/bless.py, which now refuses "
                    "unattributable input). Re-blessing a committed golden is "
                    "a tracked-file diff and a USER decision -- do not do it "
                    "to make this message go away.\n"
                    "         CANDIDATE: recapture with "
                    "tools/visual_verify/capture.py (schema_v>=2), which emits "
                    "identity.exe.sha256 plus the binary's embedded build "
                    "fingerprint.\n"
                    "         MEANWHILE: `verify.py --run <dir>` with NO "
                    "--golden still emits every region crop and contact sheet "
                    "for eyeball comparison. It just does not pretend to be a "
                    "verdict."))
        except pg.ProvenanceUnestablished as e:
            raise SystemExit(f"[vv-verify] {e}")

    # The render options of the golden and of the candidate must agree (--vary names a deliberate difference); the report carries both records.
    options_record = None
    if golden_dir is not None and not baseline:
        options_record = check_render_options(golden_dir, cap, vary or [])

    # CAPTURE-RESOLUTION-ASSERT-1 (HARNESS-HYGIENE-3). Region rects are
    # normalized fractions and diff_crops() below silently upscales a smaller
    # candidate to the golden's crop dims, so a capture taken at the wrong
    # resolution crops, grades and blesses cleanly with a fraction of the real
    # pixels. That happened (an 800x600 install vs a 4K-referenced golden) and
    # was caught only by a human reading a sidecar. Assert it here, before any
    # crop is graded -- and only when there is a golden to be measured against.
    if golden_dir is not None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
        import importlib.util as _ilu
        _spec = _ilu.spec_from_file_location(
            "check_capture_resolution",
            Path(__file__).resolve().parents[2] / "scripts" /
            "check-capture-resolution.py")
        _crm = _ilu.module_from_spec(_spec)
        _spec.loader.exec_module(_crm)
        try:
            _crm.assert_capture_resolution(golden_dir, run_dir)
        except _crm.ResolutionMismatch as _e:
            raise SystemExit(f"[vv-verify] RESOLUTION MISMATCH: {_e}")

    crops_root = run_dir / "crops"
    crops_root.mkdir(exist_ok=True)
    contact_root = run_dir / "contact_sheets"
    contact_root.mkdir(exist_ok=True)

    all_manifests = regionlib.load_all_manifests()
    pose_reports = []

    for pose in cap.get("poses", []):
        bm = pose["bookmark"]
        png_path = pose.get("primary_png")
        doc = all_manifests.get(bm)
        if doc is None:
            pose_reports.append({"bookmark": bm, "verdict": "NO_MANIFEST",
                                 "regions": [], "note": "no region manifest committed"})
            continue
        if not png_path or not Path(png_path).exists():
            pose_reports.append({"bookmark": bm, "verdict": "SUSPECT", "regions": [],
                                 "note": "primary capture PNG missing"})
            continue

        png_bytes = Path(png_path).read_bytes()
        pose_crop_dir = crops_root / bm
        pose_crop_dir.mkdir(parents=True, exist_ok=True)
        contact_dir = contact_root / bm
        contact_dir.mkdir(parents=True, exist_ok=True)

        region_rows = []
        for reg in doc["regions"]:
            rid = reg["id"]
            try:
                cw, ch, crop_png = regionlib.crop_region_png(png_bytes, reg["rect"])
            except Exception as e:
                region_rows.append({"id": rid, "verdict": "SUSPECT",
                                    "reason": f"crop failed: {e}",
                                    "severity": reg.get("severity"),
                                    "check": reg.get("check")})
                continue
            crop_path = pose_crop_dir / f"{rid}.png"
            crop_path.write_bytes(crop_png)
            # copy into the contact sheet too (candidate side)
            (contact_dir / f"{rid}__candidate.png").write_bytes(crop_png)

            row = {"id": rid, "severity": reg.get("severity"),
                   "check": reg.get("check"), "crop": str(crop_path),
                   "size": f"{cw}x{ch}"}

            golden_png = None
            if golden_dir is not None:
                gp = Path(golden_dir) / bm / f"{rid}.png"
                if gp.exists():
                    golden_png = gp.read_bytes()

            if golden_png is None:
                row["verdict"] = "NEW"
                row["reason"] = "no golden" if not baseline else "baseline seed written"
                if baseline and golden_dir is not None:
                    gp = Path(golden_dir) / bm / f"{rid}.png"
                    gp.parent.mkdir(parents=True, exist_ok=True)
                    gp.write_bytes(crop_png)
            else:
                try:
                    metrics = diff_crops(golden_png, crop_png)
                except Exception as e:
                    row["verdict"] = "SUSPECT"
                    row["reason"] = f"diff failed: {e}"
                    region_rows.append(row)
                    continue
                verdict, reason = grade(metrics, reg.get("thresholds"))
                row["verdict"] = verdict
                row["reason"] = reason
                row["metrics"] = metrics
                (contact_dir / f"{rid}__golden.png").write_bytes(golden_png)
            region_rows.append(row)

        worst = "PASS"
        for r in region_rows:
            worst = _worse(worst, r["verdict"])
        pose_reports.append({
            "bookmark": bm,
            "mission": pose.get("mission"),
            "verdict": worst,
            "primary_png": png_path,
            "crop_dir": str(pose_crop_dir),
            "contact_dir": str(contact_dir),
            "regions": region_rows,
        })

    report = {
        "kind": "visual-verify-report",
        "schema_v": 2,
        "generated_at": _now_iso(),
        "run_dir": str(run_dir),
        "golden_dir": str(golden_dir) if golden_dir else None,
        "baseline_mode": baseline,
        "render_options": options_record,
        "install": cap.get("install"),
        "exe": cap.get("exe"),
        "frame": cap.get("frame"),
        # GOLDEN-PROVENANCE-TRUTH-1: a verdict must carry what it graded, not
        # just its own conclusion. Populated only on the grading path; in
        # crop-emit / baseline mode there is no verdict to attribute.
        "graded_provenance": ({
            "golden": pg.establish_path(Path(golden_dir) / "golden.json",
                                        "golden"),
            "candidate": pg.establish(cap, "candidate capture"),
        } if (golden_dir is not None and not baseline) else None),
        "poses": pose_reports,
    }
    report["summary"] = _summarize(pose_reports)
    (run_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf-8")
    _write_md(run_dir / "report.md", report)
    return report


def _worse(a, b):
    order = ["PASS", "NEW", "NO_MANIFEST", "FLIP", "FAIL", "SUSPECT"]
    ia = order.index(a) if a in order else len(order)
    ib = order.index(b) if b in order else len(order)
    return a if ia >= ib else b


def _summarize(pose_reports):
    counts = {}
    region_counts = {}
    for p in pose_reports:
        counts[p["verdict"]] = counts.get(p["verdict"], 0) + 1
        for r in p.get("regions", []):
            region_counts[r["verdict"]] = region_counts.get(r["verdict"], 0) + 1
    exit_code = 0
    for p in pose_reports:
        exit_code = max(exit_code, VERDICT_EXIT.get(p["verdict"], 0))
    return {"pose_verdicts": counts, "region_verdicts": region_counts,
            "exit": exit_code}


def _write_md(path: Path, report: dict) -> None:
    lines = []
    lines.append(f"# Visual-verify report")
    lines.append("")
    lines.append(f"- run: `{report['run_dir']}`")
    lines.append(f"- install: `{report['install']}`")
    lines.append(f"- frame: {report['frame']}")
    gd = report["golden_dir"]
    lines.append(f"- golden: `{gd}`" if gd else "- golden: (none — baseline mode)")
    lines.append(f"- generated: {report['generated_at']}")
    ro = report.get("render_options")
    if ro:
        shown = render_options_pin.describe(ro["candidate"])
        lines.append(f"- render options: {shown} (the golden recorded the same" + (f", except --vary {','.join(ro['vary'])}" if ro["vary"] else "") + ")")
    s = report["summary"]
    lines.append("")
    lines.append(f"**Pose verdicts:** " +
                 ", ".join(f"{k}={v}" for k, v in sorted(s["pose_verdicts"].items())))
    lines.append(f"**Region verdicts:** " +
                 ", ".join(f"{k}={v}" for k, v in sorted(s["region_verdicts"].items())))
    lines.append(f"**exit:** {s['exit']}")
    lines.append("")
    for p in report["poses"]:
        lines.append(f"## {p['bookmark']}  —  **{p['verdict']}**")
        if p.get("note"):
            lines.append(f"_{p['note']}_")
            lines.append("")
            continue
        lines.append("")
        lines.append("| region | sev | verdict | mean_d | chg% | struct | reason / check |")
        lines.append("|---|---|---|---|---|---|---|")
        for r in p.get("regions", []):
            m = r.get("metrics") or {}
            md = m.get("mean_delta", "")
            cp = m.get("changed_pct", "")
            st = m.get("struct_delta", "")
            detail = r.get("reason") or ""
            if r["verdict"] in ("NEW", "PASS") and not detail:
                detail = (r.get("check") or "")[:80]
            lines.append(
                f"| {r['id']} | {r.get('severity','')} | **{r['verdict']}** | "
                f"{md} | {cp} | {st} | {detail} |")
        lines.append("")
        lines.append(f"Crops: `{p.get('crop_dir','')}` · "
                     f"contact sheet: `{p.get('contact_dir','')}`")
        lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--run", required=True, help="capture dir (has capture_run.json)")
    ap.add_argument("--golden", default=None,
                    help="golden crop dir (tests/visual/goldens/<name>), or `current` for "
                         "the golden set at the pinned capture size. Omit for "
                         "pure crop-emit; with --baseline it is seeded.")
    ap.add_argument("--baseline", action="store_true",
                    help="baseline mode: write emitted crops as the golden seed "
                         "into --golden (all regions verdict NEW).")
    ap.add_argument("--vary", default="",
                    help="comma-separated options.cfg keys that differ between the golden and this capture on purpose (any other difference is refused)")
    args = ap.parse_args(argv)

    if args.golden == "current":      # the golden set blessed at the pinned capture size (scripts/smoke_lib/render_resolution.py)
        from smoke_lib import render_resolution
        args.golden = str(_THIS.parents[2] / "tests" / "visual" / "goldens" / render_resolution.CURRENT_GOLDEN)
    golden = Path(args.golden).resolve() if args.golden else None
    if args.baseline and golden is None:
        print("[vv-verify] ERROR --baseline requires --golden (the seed dir)",
              file=sys.stderr)
        return 2

    report = verify_run(Path(args.run), golden, args.baseline, [k for k in args.vary.split(",") if k])
    s = report["summary"]
    print(f"[vv-verify] poses: " +
          ", ".join(f"{k}={v}" for k, v in sorted(s["pose_verdicts"].items())))
    print(f"[vv-verify] regions: " +
          ", ".join(f"{k}={v}" for k, v in sorted(s["region_verdicts"].items())))
    print(f"[vv-verify] report.md:   {Path(args.run).resolve() / 'report.md'}")
    print(f"[vv-verify] report.json: {Path(args.run).resolve() / 'report.json'}")
    print(f"[vv-verify] exit={s['exit']}")
    return s["exit"]


if __name__ == "__main__":
    sys.exit(main())

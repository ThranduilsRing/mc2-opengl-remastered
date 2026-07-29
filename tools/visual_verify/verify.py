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
      --golden tests/visual/goldens/v0.5.1-baseline

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
from png_io import decode_png, encode_png  # noqa: E402
import regions as regionlib  # noqa: E402

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


def verify_run(run_dir: Path, golden_dir: "Path | None", baseline: bool) -> dict:
    run_dir = Path(run_dir).resolve()
    manifest_path = run_dir / "capture_run.json"
    if not manifest_path.exists():
        raise SystemExit(f"[vv-verify] no capture_run.json in {run_dir}")
    cap = json.loads(manifest_path.read_text(encoding="utf-8"))

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
        "schema_v": 1,
        "generated_at": _now_iso(),
        "run_dir": str(run_dir),
        "golden_dir": str(golden_dir) if golden_dir else None,
        "baseline_mode": baseline,
        "install": cap.get("install"),
        "exe": cap.get("exe"),
        "frame": cap.get("frame"),
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
                    help="golden crop dir (tests/visual/goldens/<name>). Omit for "
                         "pure crop-emit; with --baseline it is seeded.")
    ap.add_argument("--baseline", action="store_true",
                    help="baseline mode: write emitted crops as the golden seed "
                         "into --golden (all regions verdict NEW).")
    args = ap.parse_args(argv)

    golden = Path(args.golden).resolve() if args.golden else None
    if args.baseline and golden is None:
        print("[vv-verify] ERROR --baseline requires --golden (the seed dir)",
              file=sys.stderr)
        return 2

    report = verify_run(Path(args.run), golden, args.baseline)
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

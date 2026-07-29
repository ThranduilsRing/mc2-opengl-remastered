#!/usr/bin/env python3
"""VERIFY-DEBT-1 -- self-test for bless.py's three refusals.

A guard that has only ever been run against GOOD input is not known to work.
This builds synthetic capture runs that are deliberately bad in one specific way
each, and asserts bless.py refuses them. It is the standing proof for:

  T1  MIN_RUNS       -- a TWO-run set is refused (the three-run all-pairs rule
                        used to be prose; nothing enforced it).
  T2  duplicate runs -- passing the same run dir 3x is self-comparison, refused.
  T3  degenerate     -- an all-black region is refused, not blessed.
  T4  unstable       -- a region that agrees on pair r1/r2 but diverges on r1/r3
                        is refused. THIS IS THE CASE A TWO-RUN CHECK BLESSES: the
                        r1/r2 pair scores 0.0000%. It is why MIN_RUNS is 3.
  T5  resolution     -- mixed capture resolutions are refused.
  T6  happy path     -- a clean 3-run set DOES bless, with tool-emitted
                        provenance (blessed_commit / capture_env / gate_state)
                        and all-pairs reproducibility recorded per region.

Run:  py -3 tools/visual_verify/bless_selftest.py
Exit 0 = every guard fired exactly where it should.
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path

_THIS = Path(__file__).resolve()
sys.path.insert(0, str(_THIS.parents[1] / "visual"))
sys.path.insert(0, str(_THIS.parent))

import regions as regionlib  # noqa: E402
import bless as blesslib  # noqa: E402
from png_io import encode_png  # noqa: E402

W, H = 400, 200
BM = "selftest_pose"

# Region rects, in normalized coords, over disjoint horizontal bands.
#   good     y 0.00-0.25   textured, identical every run   -> blesses
#   void     y 0.25-0.50   solid black every run           -> T3 degenerate
#   unstable y 0.50-0.75   identical in r1,r2; differs r3  -> T4 all-pairs
MANIFEST = {
    "bookmark": BM,
    "mission": "selftest",
    "framing": "synthetic fixture (bless_selftest.py)",
    "regions": [
        {"id": "good", "severity": "medium",
         "rect": {"mode": "norm", "x": 0.0, "y": 0.00, "w": 1.0, "h": 0.25},
         "thresholds": {"mean_delta": 4.0, "changed_pct": 2.5, "struct": 10.0},
         "check": "synthetic stable textured band"},
        {"id": "void", "severity": "critical",
         "rect": {"mode": "norm", "x": 0.0, "y": 0.25, "w": 1.0, "h": 0.25},
         "thresholds": {"mean_delta": 4.0, "changed_pct": 2.5, "struct": 10.0},
         "check": "synthetic all-black band"},
        {"id": "unstable", "severity": "high",
         "rect": {"mode": "norm", "x": 0.0, "y": 0.50, "w": 1.0, "h": 0.25},
         "thresholds": {"mean_delta": 4.0, "changed_pct": 2.5, "struct": 10.0},
         "check": "synthetic band that only diverges on the r*/r3 pairs"},
    ],
}


def _frame(run_index: int, w: int = W, h: int = H) -> bytes:
    """Synthetic frame. Deterministic per (x,y); only the `unstable` band
    (y in [0.50,0.75)) varies, and only for run index 3."""
    px = bytearray(w * h * 3)
    y_void0, y_void1 = int(0.25 * h), int(0.50 * h)
    y_uns0, y_uns1 = int(0.50 * h), int(0.75 * h)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            if y_void0 <= y < y_void1:
                r = g = b = 0                      # dead flat black
            elif y_uns0 <= y < y_uns1:
                # r1 and r2 identical; r3 shifted hard -> only r*/r3 pairs fail
                base = (x * 7 + y * 13) % 256
                r = g = b = base if run_index < 3 else (base + 128) % 256
            else:
                r = (x * 5 + y * 3) % 256
                g = (x * 11 + y * 7) % 256
                b = (x * 3 + y * 17) % 256
            px[i], px[i + 1], px[i + 2] = r, g, b
    return encode_png(w, h, 3, px)


def _make_run(root: Path, name: str, run_index: int, w: int = W, h: int = H) -> Path:
    rd = root / name
    pose_dir = rd / BM
    pose_dir.mkdir(parents=True, exist_ok=True)
    png = pose_dir / f"{BM}.png"
    png.write_bytes(_frame(run_index, w, h))
    (rd / "capture_run.json").write_text(json.dumps({
        "kind": "visual-verify-capture", "schema_v": 1,
        "install": "SELFTEST", "exe": "SELFTEST/mc2.exe",
        "exe_mtime": "2026-01-01T00:00:00Z",
        "frame": 431, "settle": 150, "deterministic_clock": True,
        "effective_mc2_env": ["MC2_SMOKE_MODE", "MC2_VISUAL_BOOKMARK_CAPTURE"],
        "poses": [{"bookmark": BM, "mission": "selftest",
                   "pose_dir": str(pose_dir), "primary_png": str(png),
                   "engine_deterministic": True, "all_present": True,
                   "engine_capture_fired": True}],
        "n_captured": 1, "n_requested": 1,
    }, indent=2), encoding="utf-8")
    return rd


FAILURES = []


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail else ""))
    if not ok:
        FAILURES.append(label)


def expect_refusal(label: str, fn, needle: str) -> None:
    try:
        fn()
    except SystemExit as e:
        msg = str(e)
        check(label, needle.lower() in msg.lower(),
              f"refused with: {msg.strip()[:130]}")
        return
    except Exception as e:  # pragma: no cover
        check(label, False, f"raised {type(e).__name__}: {e}")
        return
    check(label, False, "bless SUCCEEDED but should have refused")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="bless_selftest_"))
    orig_regions_dir = regionlib.REGIONS_DIR
    try:
        rdir = tmp / "regions"
        rdir.mkdir()
        (rdir / f"{BM}.json").write_text(json.dumps(MANIFEST, indent=2),
                                         encoding="utf-8")
        regionlib.REGIONS_DIR = rdir

        r1 = _make_run(tmp, "r1", 1)
        r2 = _make_run(tmp, "r2", 2)
        r3 = _make_run(tmp, "r3", 3)

        print("bless_selftest: proving each refusal fires on synthetic bad input")

        # T1 -- two runs is not enough.
        expect_refusal("T1 MIN_RUNS: a TWO-run set is refused",
                       lambda: blesslib.bless([r1, r2], tmp / "g_t1"),
                       "minimum is 3")

        # T2 -- duplicate run dirs are self-comparison.
        expect_refusal("T2 duplicate run dirs refused",
                       lambda: blesslib.bless([r1, r1, r1], tmp / "g_t2"),
                       "more than once")

        # T5 -- mixed capture resolution.
        r_odd = _make_run(tmp, "r_odd", 1, w=200, h=100)
        expect_refusal("T5 mixed capture resolution refused",
                       lambda: blesslib.bless([r1, r2, r_odd], tmp / "g_t5"),
                       "resolution")

        # T6 (+T3, +T4) -- the clean 3-run path.
        g = blesslib.bless([r1, r2, r3], tmp / "g_ok")
        blessed = {m["region"] for m in g["members"]}
        refused = {r["region"]: r for r in g["refused"]}

        # T3 -- degenerate all-black region refused, NOT blessed.
        check("T3 degenerate all-black region refused",
              "void" in refused and "void" not in blessed,
              refused.get("void", {}).get("reason", "")[:100])
        check("T3 degeneracy cited unique-colour floor",
              refused.get("void", {}).get("rule") == "DEGENERATE-CROP-REFUSAL-1"
              and refused.get("void", {}).get("stats", {}).get("unique_colors") == 1)
        check("T3 no crop file written for the refused region",
              not (tmp / "g_ok" / BM / "void.png").exists())

        # T4 -- the region a TWO-run check would have blessed.
        check("T4 unstable region refused on the 3-run all-pairs rule",
              "unstable" in refused and "unstable" not in blessed,
              refused.get("unstable", {}).get("reason", "")[:100])
        rep = refused.get("unstable", {}).get("reproducibility", {})
        check("T4 pair r1/r2 scored 0.0000% (a two-run bless would have PASSED it)",
              rep.get("r1_vs_r2_changed_pct") == 0.0,
              f"r1_vs_r2={rep.get('r1_vs_r2_changed_pct')} "
              f"r1_vs_r3={rep.get('r1_vs_r3_changed_pct')} "
              f"r2_vs_r3={rep.get('r2_vs_r3_changed_pct')}")
        check("T4 all 3 pairs were graded", rep.get("pairs_graded") == 3)

        # T6 -- the good region blesses, with tool-emitted provenance.
        check("T6 stable textured region blessed", "good" in blessed)
        check("T6 crop file written", (tmp / "g_ok" / BM / "good.png").exists())
        good = next(m for m in g["members"] if m["region"] == "good")
        check("T6 blessed member carries all-pairs reproducibility",
              good["reproducibility"]["pairs_graded"] == 3
              and good["reproducibility"]["runs"] == 3)
        check("T6 provenance tool-emitted: blessed_commit",
              bool(g.get("blessed_commit")), str(g.get("blessed_commit"))[:12])
        check("T6 provenance tool-emitted: capture_env.resolution",
              g["capture_env"]["resolution"] == f"{W}x{H}")
        check("T6 provenance tool-emitted: capture_env.runs == 3",
              g["capture_env"]["runs"] == 3)
        check("T6 provenance tool-emitted: gate_state present",
              "gate_state" in g and "effective_mc2_env" in g["gate_state"])
        check("T6 provenance names its emitter",
              "bless.py" in (g.get("provenance_emitted_by") or ""))
        check("T6 refusals are recorded in golden.json, not silently dropped",
              g["n_refused"] == 2 and len(g["refused"]) == 2)

        print()
        if FAILURES:
            print(f"bless_selftest: FAIL ({len(FAILURES)} guard(s) did not fire): "
                  + ", ".join(FAILURES))
            return 1
        print("bless_selftest: OK -- every refusal fired on its synthetic bad input")
        return 0
    finally:
        regionlib.REGIONS_DIR = orig_regions_dir
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

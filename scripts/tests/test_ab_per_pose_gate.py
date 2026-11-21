"""Gate-side and evidence-side pins for per-pose A/B rows (bm + bm_reverse gate bookmarks now yield 4 MAD rows, not 2)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
sys.path.insert(0, str(ROOT / "scripts"))
import ab  # noqa: E402
import merge_gate  # noqa: E402

HDR = "bookmark | FLIP mean | FLIP p99 | MAD | %>24\n"


def _png(d, bm, pose, color):
    (d / bm).mkdir(parents=True, exist_ok=True)
    Image.new("RGB", (40, 20), color).save(d / bm / f"{pose}.png")


def test_gate_pose_pair_bm_and_reverse(tmp_path):
    for side in ("a", "b"):
        _png(tmp_path / side, "bm", "bm", (10, 10, 10))
        _png(tmp_path / side, "bm", "bm_reverse", (20, 20, 20))
    rows = ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "o", [])
    assert [ab.pose_label(r["bookmark"], r["pose"]) for r in rows] == ["bm", "bm/bm_reverse"]


def test_declared_bound_applies_to_exact_name_only():
    out = HDR + "bm | n/a | n/a | 0.5000 | 0\nbm/bm_reverse | n/a | n/a | 0.5000 | 0\n"
    assert merge_gate.mad_flags(out, declared={"bm": 1.0}) == ["bm/bm_reverse=0.5000"]
    assert merge_gate.mad_flags(out, declared={"bm": 1.0, "bm/bm_reverse": 1.0}) == []
    flagged = merge_gate.mad_flags(out, declared={"bm": 0.1})
    assert any("bm=0.5000 (declared bound 0.1)" in f for f in flagged)


def test_gate_flags_missing_or_empty_png_line():
    assert merge_gate.mad_flags("[ab] bm/p2: missing or empty png (a=None b=x)\n")


def test_raw_mads_keys_per_pose(tmp_path):
    (tmp_path / "summary.json").write_text(json.dumps({"rows": [
        {"bookmark": "bm", "pose": "bm", "mad": 0.01}, {"bookmark": "bm", "pose": "p2", "mad": 0.07}]}))
    assert merge_gate.raw_mads(f"[ab] wrote {tmp_path}\n") == {"bm": 0.01, "bm/p2": 0.07}


def test_pose_missing_on_both_sides_is_a_problem(tmp_path):
    for side in ("a", "b"):
        _png(tmp_path / side, "bm", "p01", (5, 5, 5))
        (tmp_path / side / "capture_run.json").write_text(json.dumps({"poses": [{"bookmark": "bm", "all_present": False}]}))
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "o", [])
    assert any("side A capture_run.json all_present=false" in p for p in ab.compare.problems)
    assert any("side B capture_run.json all_present=false" in p for p in ab.compare.problems)

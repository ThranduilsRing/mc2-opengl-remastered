"""ab.compare pairs every pose PNG of a bookmark folder (Fable review P1-1: only the first pose was compared)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


def _png(d, bm, pose, color):
    (d / bm).mkdir(parents=True, exist_ok=True)
    Image.new("RGB", (40, 20), color).save(d / bm / f"{pose}.png")


def _run(tmp_path):
    rows = ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "o", [])
    return rows, json.loads((tmp_path / "o" / "summary.json").read_text())


def test_multi_pose_changed_pose_detected(tmp_path, capsys):
    for i, pose in enumerate(("p01", "p02", "p03")):
        _png(tmp_path / "a", "bm", pose, (10 * i, 0, 0))
        _png(tmp_path / "b", "bm", pose, (10 * i + (60 if pose == "p03" else 0), 0, 0))
    _png(tmp_path / "a", "bm", "p01_dup", (1, 1, 1))
    rows, s = _run(tmp_path)
    assert [r["pose"] for r in rows] == ["p01", "p02", "p03"]
    assert s["poses_compared"] == 3 and s["problems"] == []
    mad = {r["pose"]: r["mad"] for r in rows}
    assert mad["p01"] == 0 and mad["p02"] == 0 and mad["p03"] > 10
    assert (tmp_path / "o" / "bm" / "p03" / "montage.jpg").exists()
    out = capsys.readouterr().out
    assert "[ab] compared 3 poses" in out and "bm/p03 |" in out


def test_single_png_legacy_layout_unchanged(tmp_path):
    _png(tmp_path / "a", "bm", "bm", (10, 10, 10))
    _png(tmp_path / "b", "bm", "bm", (10, 10, 10))
    rows, s = _run(tmp_path)
    assert len(rows) == 1 and rows[0]["pose"] == "bm" and rows[0]["mad"] == 0
    assert (tmp_path / "o" / "bm" / "montage.jpg").exists()
    assert s["poses_compared"] == 1


def test_one_sided_pose_is_a_problem(tmp_path):
    _png(tmp_path / "a", "bm", "p01", (5, 5, 5))
    _png(tmp_path / "b", "bm", "p01", (5, 5, 5))
    _png(tmp_path / "a", "bm", "p02", (5, 5, 5))
    rows, s = _run(tmp_path)
    assert len(rows) == 1
    assert any("bm/p02: side B has no usable png" in p for p in s["problems"])
    assert ab.compare.problems == s["problems"]


def test_missing_bookmark_side_still_a_problem(tmp_path):
    _png(tmp_path / "a", "bm", "bm", (5, 5, 5))
    (tmp_path / "b").mkdir()
    _, s = _run(tmp_path)
    assert any("bm: side B" in p for p in s["problems"])


def test_gate_mad_flags_reads_per_pose_rows():
    import merge_gate  # scripts/ is on sys.path under pytest rootdir conftest
    out = "bookmark | FLIP mean | FLIP p99 | MAD | %>24\nbm/p01 | n/a | n/a | 0.0000 | 0.00\nbm/p02 | n/a | n/a | 9.0000 | 5.00\n"
    assert merge_gate.mad_flags(out, expected=["bm"]) == ["bm/p02=9.0000"]

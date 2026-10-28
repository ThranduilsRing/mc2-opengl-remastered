"""tools/visual_verify/ab.py is a VK-only before/after tool: no --skip-gl, two builds in, montage + MAD out."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


def _shot(d, bm, color):
    (d / bm).mkdir(parents=True)
    Image.new("RGB", (40, 20), color).save(d / bm / f"{bm}.png")


def test_compare_only(tmp_path):
    _shot(tmp_path / "a", "bm", (10, 10, 10))
    _shot(tmp_path / "b", "bm", (30, 10, 10))
    ab.main(["--compare-only", str(tmp_path / "a"), str(tmp_path / "b"), "--out-root", str(tmp_path / "o"), "--crop", "0,0,0.5,1"])
    s = json.loads((tmp_path / "o" / "summary.json").read_text())
    assert s["rows"][0]["mad"] == pytest.approx(20 / 3, abs=0.01)
    assert (tmp_path / "o" / "bm" / "montage.jpg").exists() and (tmp_path / "o" / "bm" / "diff.jpg").exists()


def test_exe_mode_stages_installs_and_runs_both(tmp_path, monkeypatch):
    inst = tmp_path / "inst"
    inst.mkdir()
    (inst / "mc2.exe").write_bytes(b"orig")
    (inst / "data.bin").write_bytes(b"d")
    ea, eb = tmp_path / "a.exe", tmp_path / "b.exe"
    ea.write_bytes(b"A")
    eb.write_bytes(b"B")
    runs = []
    monkeypatch.setattr(ab, "run_capture", lambda install, bms, out, env: runs.append((install, Path(out).name, env)))
    monkeypatch.setattr(ab, "compare", lambda *a, **k: None)
    ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(tmp_path / "o")])
    assert [Path(r[0]).name for r in runs] == ["install_a", "install_b"] and [r[1] for r in runs] == ["a", "b"]
    assert (Path(runs[0][0]) / "mc2.exe").read_bytes() == b"A" and (Path(runs[1][0]) / "mc2.exe").read_bytes() == b"B"
    assert (Path(runs[0][0]) / "data.bin").exists() and (inst / "mc2.exe").read_bytes() == b"orig"
    assert "MC2_RENDER_BACKEND=vulkan" in runs[0][2]


def test_rejects_mixed_and_missing(tmp_path):
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--install-a", "p", "--exe-b", "q", "--out-root", str(tmp_path)])
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--out-root", str(tmp_path)])

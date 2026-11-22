"""reference_index (scandir walk): same index as the walk+stat contract, excludes, and the slow-disk notice."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import asset_parity as ap  # noqa: E402


def test_index_lists_trees_sizes_and_root_fst(tmp_path):
    (tmp_path / "data" / "sub").mkdir(parents=True)
    (tmp_path / "data" / "sub" / "A.bin").write_bytes(b"12345")
    (tmp_path / "assets").mkdir()
    (tmp_path / "assets" / "x.tga").write_bytes(b"12")
    (tmp_path / "mc2res.fst").write_bytes(b"1")
    (tmp_path / "other").mkdir()
    (tmp_path / "other" / "skip.bin").write_bytes(b"1")
    idx = ap.reference_index(tmp_path)
    assert idx["data/sub/a.bin"] == ("data/sub/A.bin", 5)
    assert idx["assets/x.tga"] == ("assets/x.tga", 2)
    assert idx["mc2res.fst"] == ("mc2res.fst", 1)
    assert not any(k.startswith("other") for k in idx)


def test_slow_index_prints_prelaunch_notice(tmp_path, monkeypatch, capsys):
    (tmp_path / "data" / "d").mkdir(parents=True)
    (tmp_path / "data" / "d" / "f.bin").write_bytes(b"1")
    ticks = iter([0.0] + [10.0] * 50)
    monkeypatch.setattr(ap.time, "monotonic", lambda: next(ticks))
    ap.reference_index(tmp_path)
    assert "[PRELAUNCH] indexing" in capsys.readouterr().err

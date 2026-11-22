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


def _walk_reference(ref: Path) -> dict:
    """The pre-scandir contract: os.walk (no symlinked dirs) + os.stat per file."""
    import os
    out = {}
    for tree in ap.TREES:
        base = ref / tree
        for cur, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs
                       if not ap.is_excluded(Path(cur, d).relative_to(ref).as_posix() + "/") and not Path(cur, d).is_symlink()]
            for name in files:
                path = Path(cur, name)
                rel = path.relative_to(ref).as_posix()
                if not ap.is_excluded(rel):
                    out[ap._norm(rel)] = (rel, path.stat().st_size)
    for path in sorted(ref.glob("*" + ap.ROOT_SUFFIX)):
        out[ap._norm(path.name)] = (path.name, path.stat().st_size)
    return out


def _tree(root: Path) -> None:
    for rel, size in (("data/a/B.bin", 3), ("data/a/c/d.bin", 7), ("data/textures/_txm_cache_bak/x.bin", 2),
                      ("data/textures/txm_backup/y.bin", 2), ("data/textures/m.lst", 4), ("data/textures/keep.tga", 5),
                      ("assets/z.tga", 9), ("assets/deep/er/w.bin", 1), ("data/.cookstamp", 1)):
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x" * size)
    (root / "mc2res.fst").write_bytes(b"12")


def test_scandir_index_equals_the_walk_and_stat_index(tmp_path):
    _tree(tmp_path)
    assert ap.reference_index(tmp_path) == _walk_reference(tmp_path)


def test_excluded_subdirs_and_game_written_lists_are_not_indexed(tmp_path):
    _tree(tmp_path)
    keys = set(ap.reference_index(tmp_path))
    assert "data/textures/keep.tga" in keys
    assert not any("_txm_cache_bak" in k or "txm_backup" in k for k in keys)
    assert "data/textures/m.lst" not in keys  # GAME_WRITTEN_LST


def test_symlinked_directories_are_not_followed(tmp_path):
    import os
    import pytest
    _tree(tmp_path)
    outside = tmp_path.parent / (tmp_path.name + "-outside")
    outside.mkdir()
    (outside / "leak.bin").write_bytes(b"leak")
    try:
        os.symlink(outside, tmp_path / "data" / "linked", target_is_directory=True)
    except (OSError, NotImplementedError):
        pytest.skip("no symlink privilege on this host")
    keys = set(ap.reference_index(tmp_path))
    assert not any(k.startswith("data/linked") for k in keys)
    assert ap.reference_index(tmp_path) == _walk_reference(tmp_path)

"""--bootstrap-from: a new lane install gets the base files copied once; the deploy adds the rest."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def fake_install(root: Path) -> Path:
    for rel in ("data/missions/a.pak", "assets/x.png", "shaders/old.glsl", "cache/c.bin", "textures.fst", "SDL2.dll", "mc2.exe",
                "mc2.pdb", dp.LAUNCHER_NAME, dp.MANIFEST_NAME, "run.log", "options.cfg"):
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x")
    return root


def test_bootstrap_copies_base_once(tmp_path):
    src, target = fake_install(tmp_path / "src"), tmp_path / "new" / "lane"
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    got = {p.relative_to(target).as_posix() for p in target.rglob("*") if p.is_file()}
    assert got == {"data/missions/a.pak", "assets/x.png", "textures.fst", "SDL2.dll", "options.cfg"}
    (target / "marker").write_bytes(b"y")
    (src / "later.fst").write_bytes(b"z")
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")      # target has files: no second copy
    assert not (target / "later.fst").exists() and (target / "marker").exists()


def test_bootstrap_rejects_non_install_and_self(tmp_path):
    (tmp_path / "empty").mkdir()
    with pytest.raises(SystemExit):
        dp.bootstrap_install(str(tmp_path / "empty"), str(tmp_path / "t"), "mc2.exe", "mc2.pdb")
    src = fake_install(tmp_path / "src")
    with pytest.raises(SystemExit):
        dp.bootstrap_install(str(src), str(src), "mc2.exe", "mc2.pdb")

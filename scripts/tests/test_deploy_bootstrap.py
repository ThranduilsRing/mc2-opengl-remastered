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
                "mc2.pdb", dp.RETIRED_LAUNCHER, dp.MANIFEST_NAME, "run.log", "options.cfg"):
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x")
    return root


def test_bootstrap_copies_base_once(tmp_path):
    src, target = fake_install(tmp_path / "src"), tmp_path / "new" / "lane"
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    got = {p.relative_to(target).as_posix() for p in target.rglob("*") if p.is_file()}
    assert got == {"data/missions/a.pak", "assets/x.png", "textures.fst", "SDL2.dll", "options.cfg", "run.log"}
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


def test_native_bootstrap_omits_windows_binaries_and_owner_record(tmp_path):
    src, target = fake_install(tmp_path / "src"), tmp_path / "lane"
    (src / ".mc2_deploy_lane.json").write_text("{}")
    dp.bootstrap_install(str(src), str(target), "mc2", "mc2.pdb")
    got = {p.relative_to(target).as_posix() for p in target.rglob("*") if p.is_file()}
    assert got == {"data/missions/a.pak", "assets/x.png", "textures.fst", "options.cfg", "run.log"}


def test_concrete_detail_is_cooked_on_demand_only_when_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(dp, "CONCRETE_DETAIL_SRC", str(tmp_path))
    calls = []
    cooker = lambda cmd, **kw: (calls.append(cmd), type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})())[1]
    dp.ensure_concrete_detail(str(tmp_path), cooker)
    assert len(calls) == 1 and calls[0][-1].endswith("terrain_layers")
    for rel in dp.TERRAIN_LAYER_PAYLOAD:
        if "concrete_detail" in rel:
            (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
            (tmp_path / rel).write_bytes(b"x")
    dp.ensure_concrete_detail(str(tmp_path), cooker)
    assert len(calls) == 1


def test_grass_turf_is_cooked_when_absent_or_older_than_its_cooker(tmp_path, monkeypatch, capsys):
    import os
    monkeypatch.setattr(dp, "GRASS_TURF_SRC", str(tmp_path))
    assert dp.GRASS_TURF_REL in dp.TERRAIN_LAYER_PAYLOAD
    script = tmp_path / dp.GRASS_TURF_COOK
    script.parent.mkdir(parents=True)
    script.write_text("#")
    calls = []
    cooker = lambda cmd, **kw: (calls.append(cmd), type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})())[1]
    dp.ensure_grass_turf(str(tmp_path), cooker)
    assert len(calls) == 1 and calls[0][-1].endswith("terrain_layers") and calls[0][calls[0].index("--src") + 1] == str(tmp_path)
    out = tmp_path / dp.GRASS_TURF_REL
    out.parent.mkdir(parents=True)
    out.write_bytes(b"x")
    os.utime(script, (1_000_000, 1_000_000))
    dp.ensure_grass_turf(str(tmp_path), cooker)
    assert len(calls) == 1                       # present and newer than the cooker: kept
    os.utime(script, (4_000_000_000, 4_000_000_000))
    dp.ensure_grass_turf(str(tmp_path), cooker)
    assert len(calls) == 2                       # the cooker changed: recooked
    monkeypatch.setattr(dp, "GRASS_TURF_SRC", str(tmp_path / "nope"))
    out.unlink()
    dp.ensure_grass_turf(str(tmp_path), cooker)
    assert len(calls) == 2 and "[DEPLOY] WARN" in capsys.readouterr().out


def test_bootstrap_fails_on_critical_disk_and_leaves_no_target(tmp_path, monkeypatch):
    src, target = fake_install(tmp_path / "src"), tmp_path / "lane"
    monkeypatch.setattr(dp, "disk_space", lambda *a, **k: (3, {}))
    with pytest.raises(SystemExit):
        dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    assert not target.exists() and not Path(str(target) + ".partial").exists()


def test_bootstrap_failed_copy_leaves_no_half_install(tmp_path, monkeypatch):
    src, target = fake_install(tmp_path / "src"), tmp_path / "lane"
    def boom(*a, **k):
        raise OSError("disk full")
    monkeypatch.setattr(dp.shutil, "copy2", boom)
    with pytest.raises(OSError):
        dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    assert not target.exists() and not list(tmp_path.glob("lane.partial*"))
    monkeypatch.undo()
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    assert (target / "textures.fst").is_file() and not Path(str(target) + ".partial").exists()


def test_bootstrap_preserves_preexisting_partial_save(tmp_path):
    src, target = fake_install(tmp_path / "src"), tmp_path / "lane"
    save = tmp_path / "lane.partial/savegame/x.fit"
    save.parent.mkdir(parents=True)
    save.write_bytes(b"owner save")
    dp.bootstrap_install(str(src), str(target), "mc2.exe", "mc2.pdb")
    assert save.read_bytes() == b"owner save"


def test_concrete_cook_timeout_and_missing_source_are_loud(tmp_path, monkeypatch, capsys):
    import subprocess
    monkeypatch.setattr(dp, "CONCRETE_DETAIL_SRC", str(tmp_path / "nope"))
    dp.ensure_concrete_detail(str(tmp_path))
    assert "[DEPLOY] WARN" in capsys.readouterr().out
    monkeypatch.setattr(dp, "CONCRETE_DETAIL_SRC", str(tmp_path))
    def slow(cmd, **kw):
        raise subprocess.TimeoutExpired(cmd, 300)
    with pytest.raises(SystemExit) as ex:
        dp.ensure_concrete_detail(str(tmp_path), slow)
    assert ex.value.code != 0 and "timed out" in capsys.readouterr().err


def _failing_cooker(cmd, **kw):
    return type("R", (), {"returncode": 3, "stdout": "", "stderr": "boom"})()


def test_allow_stock_trees_downgrades_a_failed_cook_to_a_warning(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(dp, "CONCRETE_DETAIL_SRC", str(tmp_path))
    monkeypatch.setattr(dp, "ALLOW_STOCK_TREES", True)
    dp.ensure_concrete_detail(str(tmp_path), _failing_cooker)   # no SystemExit
    assert "--allow-stock-trees" in capsys.readouterr().out
    monkeypatch.setattr(dp, "ALLOW_STOCK_TREES", False)
    with pytest.raises(SystemExit):
        dp.ensure_concrete_detail(str(tmp_path), _failing_cooker)


def test_failed_cooks_exit_nonzero_instead_of_shipping_stock_data(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(dp, "CONCRETE_DETAIL_SRC", str(tmp_path))
    monkeypatch.setattr(dp, "CLIFF_ROCK_SRC", str(tmp_path))
    for call in (lambda: dp.ensure_concrete_detail(str(tmp_path), _failing_cooker),
                 lambda: dp.ensure_cliff_rock(str(tmp_path), _failing_cooker)):
        with pytest.raises(SystemExit) as ex:
            call()
        assert ex.value.code != 0
    tex = tmp_path / "data" / "textures"
    tex.mkdir(parents=True)
    for i in range(6):
        (tex / f"mat{i}_normal.tga").write_bytes(b"t")
    with pytest.raises(SystemExit) as ex:
        dp.ensure_engine_normal_cooks(str(tmp_path), _failing_cooker)
    assert ex.value.code != 0 and not (tex / "engine_normals.cookver").exists()
    assert "boom" in capsys.readouterr().err


def test_engine_normal_sidecars_are_cooked_only_when_absent_or_stale(tmp_path):
    import os
    tex = tmp_path / "data" / "textures"
    tex.mkdir(parents=True)
    for i in range(6):
        (tex / f"mat{i}_normal.tga").write_bytes(b"t")
    calls = []
    cooker = lambda cmd, **kw: (calls.append(cmd), type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})())[1]
    dp.ensure_engine_normal_cooks(str(tmp_path), cooker)
    assert len(calls) == 1 and calls[0][-3:] == ["--engine-normals", str(tex), "--force"]   # no stamp yet: full recook
    for i in range(6):
        (tex / f"mat{i}_normal.ktx2").write_bytes(b"k")
    dp.ensure_engine_normal_cooks(str(tmp_path), cooker)
    assert len(calls) == 1
    os.utime(tex / "mat3_normal.tga", (4e9, 4e9))          # a newer TGA makes its sidecar stale
    dp.ensure_engine_normal_cooks(str(tmp_path), cooker)
    assert len(calls) == 2
    (tex / "engine_normals.cookver").write_text("1\n")    # an older cook meaning recooks everything
    dp.ensure_engine_normal_cooks(str(tmp_path), cooker)
    assert len(calls) == 3 and calls[2][-1] == "--force"


def test_forest_floor_cook_is_gated_on_its_own_source(tmp_path, monkeypatch, capsys):
    ground, forest = tmp_path / "ground", tmp_path / "forest"
    ground.mkdir()
    monkeypatch.setattr(dp, "GRASS_TURF_SRC", str(ground))
    monkeypatch.setattr(dp, "FOREST_FLOOR_SRC", str(tmp_path / "missing"))
    script = tmp_path / dp.FOREST_FLOOR_COOK
    script.parent.mkdir(parents=True)
    script.write_text("#")
    calls = []
    cooker = lambda cmd, **kw: (calls.append(cmd), type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})())[1]
    dp.ensure_forest_floor(str(tmp_path), cooker)       # ground dir exists but the forest source does not: no cook
    assert not calls and "[DEPLOY] WARN" in capsys.readouterr().out
    forest.mkdir()
    monkeypatch.setattr(dp, "GRASS_TURF_SRC", str(tmp_path / "nope"))
    monkeypatch.setattr(dp, "FOREST_FLOOR_SRC", str(forest))
    dp.ensure_forest_floor(str(tmp_path), cooker)       # ground dir absent but the forest source exists: cooks from it
    assert len(calls) == 1 and calls[0][calls[0].index("--src") + 1] == str(forest)

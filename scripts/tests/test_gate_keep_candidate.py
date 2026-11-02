"""--keep-candidate: the deployed candidate survives the gate with campaign mods staged; --drop-candidate / publish remove it."""
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate


def _mods_source(tmp_path):
    src = tmp_path / "mods_src"
    for name in gate.CANDIDATE_MODS:
        (src / name / "data").mkdir(parents=True)
        (src / name / "data" / "x.fit").write_text(name)
    return src


def test_flags_parse_and_default_off():
    ap_src = Path(gate.__file__).read_text(encoding="utf-8")
    assert '"--keep-candidate"' in ap_src and '"--drop-candidate"' in ap_src
    assert "ninja_build.py mc2 mc2_asset_viewer -j\" +" in ap_src  # -jN, not '-j N'


def test_keep_stock_copy_stages_linked_mods_and_records(tmp_path):
    install = tmp_path / "seed-stock-abc"
    install.mkdir()
    (install / "mc2.exe").write_text("exe")
    src = _mods_source(tmp_path)
    old = gate.CANDIDATE_MODS_SOURCE
    gate.CANDIDATE_MODS_SOURCE = src
    try:
        root = tmp_path / "root"
        (root / "build64-ninja").mkdir(parents=True)
        cand = tmp_path / "merge-gate-candidate"
        cand.mkdir()
        (cand / "stale").write_text("old")
        kept = gate.keep_stock_copy(install, "a" * 40, candidate=cand, root=root)
    finally:
        gate.CANDIDATE_MODS_SOURCE = old
    assert kept == cand and not install.exists() and not (cand / "stale").exists()
    for name in gate.CANDIDATE_MODS:
        staged = cand / "mods" / name / "data" / "x.fit"
        assert staged.read_text() == name and os.path.samefile(staged, src / name / "data" / "x.fit")
    assert json.loads((root / gate.CANDIDATE_MARK).read_text())["path"] == str(cand)
    gate.drop_candidate(cand, root)
    assert not cand.exists() and not (root / gate.CANDIDATE_MARK).exists()


def test_missing_mod_source_is_an_error(tmp_path):
    (tmp_path / "inst").mkdir()
    try:
        gate.stage_candidate_mods(tmp_path / "inst", source=tmp_path / "nope")
    except RuntimeError as exc:
        assert "MechCommanderOmnitech" in str(exc)
    else:
        raise AssertionError("expected RuntimeError")


def test_stock_copy_keep_leaves_the_copy(tmp_path):
    seed = tmp_path / "seed"
    seed.mkdir()
    (seed / "mc2.exe").write_text("x")
    with gate.stock_copy(seed, keep=True) as clone:
        pass
    assert clone.is_dir()

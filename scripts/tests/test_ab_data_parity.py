"""ab.py data parity: per-side data-tree fingerprint, DATA same/differs report, CROSS-DATA refusal/banner, one shared stage tree."""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


def _install(root: Path, shrub: bytes = b"v1", exe: bytes = b"exe") -> Path:
    files = {"mc2.exe": exe, "shaders/vulkan/x.spv": b"spv" + exe, "data/a.fst": b"a", "data/veg/shrub.ktx2": shrub,
             "data/missions/m.fit": b"fit"}
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        os.utime(p, (1_700_000_000, 1_700_000_000))
    return root


def test_same_data_different_exe_is_same(tmp_path):
    a, b = _install(tmp_path / "a", exe=b"old"), _install(tmp_path / "b", exe=b"new")
    dp = ab.data_parity(a, b)
    assert dp["same"] and not dp["cross_data"] and dp["sha_a"] == dp["sha_b"]
    assert ab.data_parity_lines(dp)[0].startswith("[ab] DATA: same")


def test_different_data_reports_paths(tmp_path):
    a, b = _install(tmp_path / "a"), _install(tmp_path / "b", shrub=b"greener-shrub")
    (b / "data/extra.ktx2").write_bytes(b"x")
    dp = ab.data_parity(a, b, full_install_a=True)
    assert not dp["same"] and dp["cross_data"] and dp["sha_a"] != dp["sha_b"]
    assert dp["differing"] == ["data/extra.ktx2", "data/veg/shrub.ktx2"]
    lines = ab.data_parity_lines(dp)
    assert lines[0].startswith("[ab] DATA: differs (2 files)")
    assert any("data/veg/shrub.ktx2" in ln for ln in lines) and "CROSS-DATA" in lines[-1]


def test_differing_list_capped_at_ten_in_print(tmp_path):
    a, b = _install(tmp_path / "a"), _install(tmp_path / "b")
    for i in range(15):
        (b / f"data/n{i:02}.bin").write_bytes(b"x")
    lines = ab.data_parity_lines(ab.data_parity(a, b))
    assert "15 files" in lines[0] and len([ln for ln in lines if ln.startswith("[ab]   ")]) == 10


def test_exe_and_shader_changes_are_not_data(tmp_path):
    a, b = _install(tmp_path / "a", exe=b"1"), _install(tmp_path / "b", exe=b"22")
    assert ab.data_parity(a, b)["same"]


def test_main_refuses_cross_data_without_flag_and_allows_with(tmp_path, monkeypatch):
    a, b = _install(tmp_path / "a"), _install(tmp_path / "b", shrub=b"other")
    monkeypatch.setattr(ab, "resolve_installs", lambda args, out: (str(a), str(b)))
    monkeypatch.setattr(ab, "ensure_saved_bookmarks", lambda *x, **k: None)
    monkeypatch.setattr(ab, "bookmark_parity_problems", lambda *x, **k: [])
    monkeypatch.setattr(ab, "payload_identity", lambda d: {"commit": "c" * 40})
    monkeypatch.setattr(ab, "base_problems", lambda *x, **k: [])
    monkeypatch.setattr(ab, "run_capture", lambda *x, **k: None)
    monkeypatch.setattr(ab, "stamp_identity", lambda *x, **k: None)
    monkeypatch.setattr(ab, "capture_commit_report", lambda *x, **k: [])
    seen = {}
    monkeypatch.setattr(ab, "compare", lambda *x, **k: seen.update(k))
    argv = ["--bookmarks", "x", "--install-a", str(a), "--install-b", str(b), "--out-root", str(tmp_path / "o")]
    with pytest.raises(SystemExit, match="different data trees"):
        ab.main(argv)
    with pytest.raises(SystemExit) as ex:
        ab.main(argv + ["--full-install-a"])
    assert ex.value.code == 0 and seen["data_parity"]["cross_data"] and seen["data_parity"]["full_install_a"]


def test_exe_swap_stages_one_shared_data_tree(tmp_path):
    """Default mode: A is B's tree with only exe/shaders swapped, so the data fingerprints match."""
    inst = _install(tmp_path / "inst", exe=b"new")
    other = _install(tmp_path / "other", shrub=b"stale", exe=b"old")
    staged_a = tmp_path / "stage" / "install_a"
    ab.stage_tree(inst, staged_a)
    (staged_a / "mc2.exe").unlink()
    (staged_a / "mc2.exe").write_bytes((other / "mc2.exe").read_bytes())
    dp = ab.data_parity(staged_a, inst)
    assert dp["same"] and (staged_a / "data/veg/shrub.ktx2").read_bytes() == b"v1"
    assert not ab.data_parity(other, inst)["same"]


def test_montage_banner_and_summary_record_cross_data(tmp_path):
    for side in "ab":
        (tmp_path / side / "bm").mkdir(parents=True)
        Image.new("RGB", (200, 100), (0, 0, 0)).save(tmp_path / side / "bm" / "bm.png")
    dp = {"cross_data": True, "same": False, "sha_a": "a", "sha_b": "b"}
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "plain", [], ["bm"])
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "xd", [], ["bm"], data_parity=dp)
    plain = Image.open(tmp_path / "plain" / "bm" / "montage.jpg").convert("RGB")
    stamped = Image.open(tmp_path / "xd" / "bm" / "montage.jpg").convert("RGB")
    assert plain.getpixel((10, 10)) != stamped.getpixel((10, 10))
    import json
    summ = json.loads((tmp_path / "xd" / "summary.json").read_text())
    assert summ["cross_data"] is True and summ["data_parity"]["sha_b"] == "b"

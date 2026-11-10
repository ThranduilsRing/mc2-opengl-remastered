"""ab.py staging: hardlinked on the install's own volume, game-written files copied, stage deleted at the end, disk guard."""
from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


def _install(root: Path) -> Path:
    for rel, data in {
        "mc2.exe": b"exe", "shaders/vulkan/x.spv": b"spv", "data/big.fst": b"shared data", "cache/content-model/m.bin": b"cooked",
        "options.cfg": b"cfg", "imgui.ini": b"ini", "run.log": b"log", "saved_bookmarks/x.json": b"{}",
        "cache/vulkan/p.bin": b"pipe", "debug_state/trace.jsonl": b"t", "crashes/d.dmp": b"dump",
    }.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return root


def test_stage_hardlinks_shared_data_and_copies_game_written(tmp_path):
    src = _install(tmp_path / "inst")
    stats = ab.stage_tree(src, tmp_path / "stage")
    assert not stats["fallback"]
    for rel in ("data/big.fst", "cache/content-model/m.bin", "mc2.exe"):
        assert os.stat(tmp_path / "stage" / rel).st_nlink > 1, rel
        assert os.path.samestat(os.stat(src / rel), os.stat(tmp_path / "stage" / rel)), rel


def test_game_written_files_are_real_copies(tmp_path):
    src = _install(tmp_path / "inst")
    ab.stage_tree(src, tmp_path / "stage")
    for rel in ("options.cfg", "imgui.ini", "run.log", "saved_bookmarks/x.json", "cache/vulkan/p.bin"):
        staged = tmp_path / "stage" / rel
        assert staged.read_bytes() == (src / rel).read_bytes() and not os.path.samestat(os.stat(src / rel), os.stat(staged)), rel
    # output-only dirs are staged empty
    assert list((tmp_path / "stage" / "debug_state").iterdir()) == [] and list((tmp_path / "stage" / "crashes").iterdir()) == []
    (tmp_path / "stage" / "options.cfg").write_bytes(b"changed by the game")
    assert (src / "options.cfg").read_bytes() == b"cfg"


def test_link_failure_falls_back_to_copy(tmp_path, monkeypatch, capsys):
    src = _install(tmp_path / "inst")
    monkeypatch.setattr(os, "link", lambda *a: (_ for _ in ()).throw(OSError("no links")))
    stats = ab.stage_tree(src, tmp_path / "stage")
    assert stats["fallback"] and stats["linked"] == 0
    assert (tmp_path / "stage" / "data/big.fst").read_bytes() == b"shared data"
    assert capsys.readouterr().out.count("staging by copy") == 1


def _stage_setup(tmp_path, monkeypatch):
    inst = _install(tmp_path / "inst")
    import hashlib
    rows = "".join(f"{p},{hashlib.sha256((inst / p).read_bytes()).hexdigest()},1,{'a' * 40}\n" for p in ["mc2.exe", "shaders/vulkan/x.spv"])
    (inst / ab.MANIFEST).write_text("manifest_version,v1\nrelpath,sha256,bytes,src_commit\n" + rows)
    monkeypatch.setattr(ab, "exe_commit", lambda *a: "a" * 40)
    monkeypatch.setattr(ab, "base_problems", lambda *a, **k: [])
    monkeypatch.setattr(ab, "compare", lambda *a, **k: None)
    monkeypatch.setattr(ab, "commit_order_problem", lambda *a: None)
    ea, eb = tmp_path / "a.exe", tmp_path / "b.exe"
    ea.write_bytes(b"A")
    eb.write_bytes(b"B")
    return inst, ea, eb


@pytest.mark.parametrize("fail", [False, True])
def test_stage_removed_after_success_and_failure(tmp_path, monkeypatch, fail):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    seen = []

    def capture(install, bms, out, env):
        seen.append(Path(install))
        assert Path(install).is_dir()
        if fail and len(seen) == 2:
            sys.exit("capture failed")
    monkeypatch.setattr(ab, "run_capture", capture)
    out = tmp_path / "o"
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(out)])
    assert len(seen) == 2
    assert not any(p.exists() for p in seen)
    assert not (tmp_path / "_ab-stage").exists()  # the empty stage root goes too
    assert (inst / "data/big.fst").read_bytes() == b"shared data"  # the source install is untouched


def test_keep_stage_leaves_the_stage(tmp_path, monkeypatch):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    seen = []
    monkeypatch.setattr(ab, "run_capture", lambda install, *a: seen.append(Path(install)))
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst),
                 "--out-root", str(tmp_path / "o"), "--keep-stage"])
    assert seen and all(p.is_dir() for p in seen)


def test_user_supplied_installs_are_never_deleted(tmp_path, monkeypatch):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    monkeypatch.setattr(ab, "run_capture", lambda *a: None)
    monkeypatch.setattr(ab, "seed_install", lambda *a, **k: None)
    monkeypatch.setattr(ab, "payload_identity", lambda d: {"commit": "a" * 40, "payload": {}, "manifest": {}})
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--install-a", str(inst), "--install-b", str(inst), "--out-root", str(tmp_path / "o")])
    assert (inst / "mc2.exe").exists()


def test_disk_guard_refuses_and_prints_numbers(tmp_path, capsys):
    free = ab.shutil.disk_usage(tmp_path).free
    with pytest.raises(SystemExit) as e:
        ab.disk_guard(tmp_path / "nonexistent" / "stage", 0, min_free_gb=free / ab.GB + 1)
    assert "DISK_GUARD" in str(e.value) and "refused" in str(e.value) and "free" in str(e.value)
    ab.disk_guard(tmp_path / "stage", 1, min_free_gb=0)
    assert "DISK_GUARD" in capsys.readouterr().out


def test_disk_guard_counts_stage_size_and_blocks_main(tmp_path, monkeypatch):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    free = ab.shutil.disk_usage(tmp_path).free
    monkeypatch.setattr(ab, "DISK_GUARD_FREE_GB", 0)
    monkeypatch.setattr(ab, "estimate_stage_bytes", lambda *a: free + ab.GB)
    monkeypatch.setattr(ab, "run_capture", lambda *a: pytest.fail("staged past the disk guard"))
    with pytest.raises(SystemExit, match="DISK_GUARD"):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(tmp_path / "o")])
    assert not (tmp_path / "_ab-stage" / "o" / "install_a").exists()


def test_estimate_is_only_the_copied_files_on_one_volume(tmp_path):
    inst = _install(tmp_path / "inst")
    est = ab.estimate_stage_bytes(inst, tmp_path / "stage")
    assert est == len(b"cfg") + len(b"ini") + len(b"log") + len(b"{}") + len(b"pipe")

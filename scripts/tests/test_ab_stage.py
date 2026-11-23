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
        "mc2.exe": b"exe", "top.fst": b"fst", "shaders/vulkan/x.spv": b"spv", "data/big.fst": b"shared data", "cache/content-model/m.bin": b"cooked",
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
    for rel in ("data/big.fst", "cache/content-model/m.bin"):
        assert os.stat(tmp_path / "stage" / rel).st_nlink > 1, rel
        assert os.path.samestat(os.stat(src / rel), os.stat(tmp_path / "stage" / rel)), rel


def test_game_written_files_are_real_copies(tmp_path):
    src = _install(tmp_path / "inst")
    ab.stage_tree(src, tmp_path / "stage")
    for rel in ("mc2.exe", "top.fst", "options.cfg", "imgui.ini", "run.log", "saved_bookmarks/x.json", "cache/vulkan/p.bin"):
        staged = tmp_path / "stage" / rel
        assert staged.read_bytes() == (src / rel).read_bytes() and not os.path.samestat(os.stat(src / rel), os.stat(staged)), rel
    # output-only dirs are staged empty
    assert list((tmp_path / "stage" / "debug_state").iterdir()) == [] and list((tmp_path / "stage" / "crashes").iterdir()) == []
    (tmp_path / "stage" / "options.cfg").write_bytes(b"changed by the game")
    assert (src / "options.cfg").read_bytes() == b"cfg"


def test_rewritten_data_caches_are_copies_not_links(tmp_path):
    """terrtxm.cpp:1496/1673 File::create (truncate) the .lst/.txm in data/textures; msl_anim.cpp:568 rewrites data/tgl/*.agl."""
    src = _install(tmp_path / "inst")
    rewritten = ("data/textures/terrain.lst", "data/textures/00000001.00000002.txm", "data/tgl/mech.agl")
    for rel in rewritten:
        (src / rel).parent.mkdir(parents=True, exist_ok=True)
        (src / rel).write_bytes(b"orig")
    ab.stage_tree(src, tmp_path / "stage")
    for rel in rewritten:
        staged = tmp_path / "stage" / rel
        assert staged.read_bytes() == b"orig" and not os.path.samestat(os.stat(src / rel), os.stat(staged)), rel
        staged.write_bytes(b"")
        assert (src / rel).read_bytes() == b"orig", rel
    assert os.path.samestat(os.stat(src / "data/big.fst"), os.stat(tmp_path / "stage" / "data/big.fst"))


def test_link_failure_falls_back_to_copy(tmp_path, monkeypatch, capsys):
    src = _install(tmp_path / "inst")
    monkeypatch.setattr(os, "link", lambda *a: (_ for _ in ()).throw(OSError("no links")))
    stats = ab.stage_tree(src, tmp_path / "stage")
    assert stats["fallback"] and stats["linked"] == 0
    assert (tmp_path / "stage" / "data/big.fst").read_bytes() == b"shared data"
    assert capsys.readouterr().out.count("staging by copy") == 1


def _stage_setup(tmp_path, monkeypatch):
    monkeypatch.setattr(ab.shutil, "disk_usage", lambda p: type("Usage", (), {"free": 100 * 2**30})())
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

    def capture(install, bms, out, env, **kw):
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
    assert all(not p.parent.exists() for p in seen)  # private run directory goes; shared parent stays
    assert (inst / "data/big.fst").read_bytes() == b"shared data"  # the source install is untouched


@pytest.mark.parametrize("scratch", [False, True])
def test_stage_roots_are_private_even_with_same_output(tmp_path, monkeypatch, scratch):
    from types import SimpleNamespace
    monkeypatch.delenv("MC2_LANE_SCRATCH", raising=False)
    if scratch:
        monkeypatch.setenv("MC2_LANE_SCRATCH", str(tmp_path / "scratch"))
    monkeypatch.setattr(ab, "disk_guard", lambda *a: None)
    monkeypatch.setattr(ab, "estimate_stage_bytes", lambda *a: 0)
    args = SimpleNamespace(stage_root=None)
    roots = [ab._stage_root(args, tmp_path / "same-output", tmp_path / "install", 1) for _ in range(2)]
    assert roots[0] != roots[1]
    assert roots[0].parent == tmp_path / ("scratch" if scratch else "_ab-stage")
    for root in roots:
        (root / "install_a").mkdir(parents=True)
    ab.cleanup_stage([roots[0] / "install_a"])
    assert not roots[0].exists() and roots[1].is_dir() and roots[0].parent.is_dir()
    args.stage_root = str(tmp_path / "explicit")
    assert ab._stage_root(args, tmp_path / "same-output", tmp_path / "install", 1).parent == tmp_path / "explicit"


@pytest.mark.parametrize("flags,size", [([], (3840, 2160)), (["--res", "1080"], (1920, 1080)),
    (["--env", "MC2_WIDTH=640", "--env", "MC2_HEIGHT=360"], (640, 360))])
def test_capture_resolution_defaults_and_opt_out(tmp_path, monkeypatch, flags, size):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    seen = []
    monkeypatch.setattr(ab, "run_capture", lambda install, bms, out, env, **kw: seen.append(ab.requested_size(env)))
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst),
                 "--out-root", str(tmp_path / "out"), *flags])
    assert seen == [size, size]


def test_keep_stage_leaves_the_stage(tmp_path, monkeypatch):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    seen = []
    monkeypatch.setattr(ab, "run_capture", lambda install, *a, **kw: seen.append(Path(install)))
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst),
                 "--out-root", str(tmp_path / "o"), "--keep-stage"])
    assert seen and all(p.is_dir() for p in seen)


def test_user_supplied_installs_are_never_deleted(tmp_path, monkeypatch):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    monkeypatch.setattr(ab, "run_capture", lambda *a, **kw: None)
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
    monkeypatch.setattr(ab, "run_capture", lambda *a, **kw: pytest.fail("staged past the disk guard"))
    with pytest.raises(SystemExit, match="DISK_GUARD"):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(tmp_path / "o")])
    assert not (tmp_path / "_ab-stage" / "o" / "install_a").exists()


def test_estimate_is_only_the_copied_files_on_one_volume(tmp_path):
    inst = _install(tmp_path / "inst")
    est = ab.estimate_stage_bytes(inst, tmp_path / "stage")
    assert est == len(b"exe") + len(b"fst") + len(b"cfg") + len(b"ini") + len(b"log") + len(b"{}") + len(b"pipe")


def test_shaders_a_stages_side_a_with_the_other_shaders_and_fixes_the_manifest(tmp_path, monkeypatch):
    import hashlib
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    v04 = tmp_path / "v04"
    (v04 / "shaders/vulkan").mkdir(parents=True)
    (v04 / "shaders/vulkan/x.spv").write_bytes(b"v04 spv")
    seen = {}

    def capture(install, bms, out, env, **kw):
        if Path(out).name == "a":
            ident = ab.payload_identity(install)  # the rewritten manifest verifies the swapped bytes
            seen.update(spv=(Path(install) / "shaders/vulkan/x.spv").read_bytes(), sha=ident["payload"]["shaders/vulkan/x.spv"])
        else:
            seen["b_spv"] = (Path(install) / "shaders/vulkan/x.spv").read_bytes()
    monkeypatch.setattr(ab, "run_capture", capture)
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst),
                 "--shaders-a", str(v04), "--out-root", str(tmp_path / "o")])
    assert seen["spv"] == b"v04 spv" and seen["sha"] == hashlib.sha256(b"v04 spv").hexdigest()
    assert seen["b_spv"] == b"spv" and (inst / "shaders/vulkan/x.spv").read_bytes() == b"spv"  # B and the source are untouched


def test_shaders_a_without_shaders_dir_refuses(tmp_path):
    with pytest.raises(SystemExit, match="no shaders"):
        ab.shaders_parent_of(tmp_path / "nothing")


def test_capture_commit_report_warns_when_b_is_not_head():
    ok = ab.capture_commit_report("a" * 40, "b" * 40, "b" * 40)
    assert len(ok) == 1 and "A=" + "a" * 40 in ok[0] and "B=" + "b" * 40 in ok[0]
    bad = ab.capture_commit_report("a" * 40, "c" * 40, "b" * 40)
    assert len(bad) == 2 and "WARNING" in bad[1] and "STALE" in bad[1]


def test_main_prints_commit_report_after_capture(tmp_path, monkeypatch, capsys):
    inst, ea, eb = _stage_setup(tmp_path, monkeypatch)
    monkeypatch.setattr(ab, "run_capture", lambda *a, **kw: None)
    monkeypatch.setattr(ab, "worktree_head", lambda: "f" * 40)
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(tmp_path / "o")])
    out = capsys.readouterr().out
    assert "CAPTURE src_commit A=" in out and "STALE" in out


def test_cleanup_reports_a_stuck_file_instead_of_raising(tmp_path, monkeypatch, capsys):
    d = tmp_path / "_ab-stage" / "ab_x" / "install_a"
    d.mkdir(parents=True)

    def stuck_rmtree(path, onexc):  # Windows refuses to delete a file mapped by another running game
        onexc(lambda p: (_ for _ in ()).throw(PermissionError("mapped")), str(path / "f.dll"), None)
    monkeypatch.setattr(ab.shutil, "rmtree", stuck_rmtree)
    ab.cleanup_stage([d])
    assert "NOT fully removed" in capsys.readouterr().out


def test_disk_guard_reserve_env_override(tmp_path, monkeypatch, capsys):
    """MC2_AB_DISK_RESERVE_GB replaces the 30 GB PC reserve (the box sets it); an explicit min_free_gb still wins."""
    free = ab.shutil.disk_usage(tmp_path).free
    monkeypatch.setenv("MC2_AB_DISK_RESERVE_GB", str(free / ab.GB + 1))
    with pytest.raises(SystemExit, match="DISK_GUARD"):
        ab.disk_guard(tmp_path / "stage", 0)
    monkeypatch.setenv("MC2_AB_DISK_RESERVE_GB", "0")
    ab.disk_guard(tmp_path / "stage", 1)
    assert "-> ok" in capsys.readouterr().out
    ab.disk_guard(tmp_path / "stage", 1, min_free_gb=0)


def test_data_tree_skips_linux_top_level_binary(tmp_path):
    """The box's extensionless Linux `mc2` (and .so/.debug) is the exe side of an A/B, never fingerprinted data."""
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "a.fit").write_text("x")
    for name in ("mc2", "mc2.exe", "mc2.pdb", "libfoo.so", "mc2.debug"):
        (tmp_path / name).write_bytes(b"bin")
    files = ab.data_tree_files(tmp_path)
    assert "data/a.fit" in files
    assert not {"mc2", "mc2.exe", "mc2.pdb", "libfoo.so", "mc2.debug"} & set(files)

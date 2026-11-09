"""deploy_payload: a fresh Ninja tree is seeded with its runtime DLLs from real sources; --target names the right flag."""
import subprocess
import sys
import pytest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import console_child  # noqa: E402
import deploy_payload as dp  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def test_seed_build_dlls_from_3rdparty_and_redist(tmp_path):
    if sys.platform != "win32":
        return
    build = tmp_path / "build"
    build.mkdir()
    copied = dp.seed_build_dlls(str(build), str(ROOT))
    expect = [n for n in ("SDL2.dll", "vcruntime140.dll") if any((Path(d) / n).is_file() for d in dp.dll_source_dirs(str(ROOT)))]
    if not expect:
        pytest.skip("no DLL source dirs here (3rdparty/lib is git-ignored; a bare clone has none)")
    assert set(expect) <= set(copied)
    assert all((build / n).is_file() for n in expect)
    assert dp.seed_build_dlls(str(build), str(ROOT)) == []  # idempotent


def test_dll_sources_are_this_repo_or_the_toolchain():
    for d in dp.dll_source_dirs(str(ROOT)):
        norm = d.replace("\\", "/")
        assert norm.startswith(str(ROOT).replace("\\", "/")) or "Redist/MSVC" in norm


def test_target_path_names_positional_arg():
    r = subprocess.run([sys.executable, str(ROOT / "scripts/deploy_payload.py"), "--target", "E:/mc2-installs/lane9"],
                       capture_output=True, text=True, timeout=60, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 2 and "positional target_dir" in r.stderr and "preset name" in r.stderr


def test_cache_skip_hashes_destination_bytes_not_size(tmp_path, monkeypatch):
    """p29 P1: an unchanged source plus a same-size corrupted destination must be re-copied, not certified from the cache."""
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda t: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda t: None)
    src, target = tmp_path / "src.bin", tmp_path / "target"
    target.mkdir()
    src.write_bytes(b"good-bytes")
    dst = target / "data.bin"
    dst.write_bytes(b"good-bytes")
    stamp = dp.source_stamp(str(src))
    cache = {"data.bin": (dp.sha256_file(str(src)), stamp[0], stamp[1])}
    items = [(str(src), "data.bin", "data")]
    dp.deploy(items, str(target), False, cache=cache)
    assert dst.read_bytes() == b"good-bytes"
    dst.write_bytes(b"evil-bytes!"[:10])   # same length as the cached row
    assert dst.stat().st_size == stamp[0]
    hashes = dp.deploy(items, str(target), False, cache=cache)
    assert dst.read_bytes() == b"good-bytes"
    assert hashes["data.bin"][0] == dp.sha256_file(str(src))


def test_deploy_logs_hash_and_copy_phases(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda t: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda t: None)
    src = tmp_path / "source"
    src.write_bytes(b"payload")
    dp.deploy([(str(src), "data.bin", "data")], str(tmp_path / "target"), False)
    out = capsys.readouterr().out
    assert "phase hash/verify seconds=" in out and "phase copy seconds=" in out
    assert (tmp_path / "target/data.bin").read_bytes() == b"payload"


@pytest.mark.parametrize("kit_enabled", [True, False])
def test_run_deploy_logs_stock_receipts_and_kit_cook(tmp_path, monkeypatch, capsys, kit_enabled):
    import kit_payload
    from smoke_lib import render_options_pin
    build, target = tmp_path / "build", tmp_path / "target"
    build.mkdir()
    target.mkdir()
    (build / "mc2").write_bytes(b"exe")
    for name in ("seed_build_dlls", "check_exe_fresh", "check_free_space_for_payload", "check_hardlink_isolation",
                 "ensure_engine_normal_cooks", "write_manifest"):
        monkeypatch.setattr(dp, name, lambda *a, **kw: None)
    monkeypatch.setattr(dp, "git_head", lambda *a: "head")
    monkeypatch.setattr(dp, "deploy_inputs", lambda *a: {})
    monkeypatch.setattr(dp, "record_deploy_inputs", lambda *a: None)
    monkeypatch.setattr(dp, "require_head_exe", lambda *a: {"dirty": False})
    monkeypatch.setattr(dp, "check_install_complete", lambda *a: [])
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda *a: {})
    monkeypatch.setattr(dp, "enumerate_payload", lambda *a, **kw: [(str(build / "mc2"), "mc2", "exe")])
    monkeypatch.setattr(render_options_pin, "record", lambda *a: {"values": {"RemasteredBuildings": kit_enabled}})
    monkeypatch.setattr(kit_payload, "payload", lambda *a, **kw: ({"fake": True}, []))
    monkeypatch.setattr(kit_payload, "build_importer", lambda *a: "importer")
    phases = []
    monkeypatch.setattr(kit_payload, "reconcile_stock_textures", lambda *a, **kw: phases.append("stock"))
    monkeypatch.setattr(kit_payload, "finish", lambda *a, **kw: phases.append("cook"))
    monkeypatch.setattr(dp.subprocess, "run", lambda *a, **kw: SimpleNamespace(stdout=""))
    args = SimpleNamespace(allow_stale_exe=False, build64_root=None, classic_assets=False, skip_kit=False,
                           require_clean=False, full_verify=False, allow_stale_pdb=False, preserve=[])
    dp.run_deploy(args, str(tmp_path), str(build), str(target), "mc2", "mc2.pdb")
    out = capsys.readouterr().out
    assert all(f"phase {phase} seconds=" in out for phase in ("hash/verify", "copy", "stock receipts", "kit cook"))
    assert phases == (["stock", "cook", "cook"] if kit_enabled else [])

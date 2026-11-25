"""deploy_payload: a fresh Ninja tree is seeded with its runtime DLLs from real sources; --target names the right flag."""
import subprocess
import errno
import json
import os
import sys
import pytest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import console_child  # noqa: E402
import deploy_payload as dp  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("rel", ["data/ui.fit", "data/editor.csv", "data/editor.txt",
                                  "tools/mc2_import/shims/corebrain.abx", "run-mc2.bat",
                                  "scripts/tool.py", "data/config.json"])
def test_text_deploy_preserves_source_bytes(tmp_path, monkeypatch, rel):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda t: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda t: None)
    src, target = tmp_path / "source", tmp_path / "install"
    for content in (b"first\nsecond\n", b"first\r\nsecond\r\n"):
        src.write_bytes(content)
        hashes = dp.deploy([(str(src), rel, "support")], str(target), False)
        assert (target / rel).read_bytes() == content
        assert hashes[rel][:2] == (dp.sha256_file(src), len(content))


def test_deploy_preserves_asset_declaration_and_shader_publication(tmp_path, monkeypatch):
    import asset_parity
    import exe_fingerprint
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda t: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda t: None)
    repo, target = tmp_path / "repo", tmp_path / "install"
    files = {"build64-ninja/mc2.exe": b"EXE", "build64-ninja/shaders/vulkan/test.spv": b"SPV",
             "shaders/spv/spirv_index.json": b'{"index": 1}\r\n',
             "shaders/spv/spirv_package.json": b'{"package": 1}\r\n',
             "data/art/asset_sizes.csv": (ROOT / "data/art/asset_sizes.csv").read_bytes()}
    items = []
    for rel, data in files.items():
        src = repo / rel
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_bytes(data)
        items.append((str(src), rel.removeprefix("build64-ninja/"), "support"))
    hashes = dp.deploy(items, str(target), False)
    head = "a" * 40
    dp.write_manifest(target, hashes, head)
    declared = {"data/art/asset_sizes.csv": json.loads((ROOT / "intentional-asset-changes.json").read_text())[
        "data/art/asset_sizes.csv"]}
    assert declared.keys() <= asset_parity.owned_paths(target, declared=declared)
    monkeypatch.setattr(exe_fingerprint, "extract", lambda p: dict(present=True, dirty=0, sha=head))
    dp.verify_published_identity(target, repo, head)


@pytest.mark.parametrize("drop_editor_file", [False, True])
def test_game_editor_game_ownership_lifecycle(tmp_path, monkeypatch, drop_editor_file):
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda t: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda t: None)
    target = tmp_path / "install"
    game = tmp_path / "game-source"
    game.write_bytes(b"game bytes")
    game_items = [(str(game), "mc2.exe", "exe")]
    def publish(items, deploying_target):
        hashes = dp.deploy(items, str(target), False, deploying_target=deploying_target)
        dp.write_manifest(target, hashes, "a" * 40)
        return hashes
    publish(game_items, "game")
    items, rows = [], {}
    for rel in ("Mission Editor.exe", "Mission Editor.pdb", "run-editor.bat", "tools/terrain_gen/tool.py"):
        path = tmp_path / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"editor bytes")
        rows[rel] = (dp.sha256_file(path), path.stat().st_size)
        items.append((str(path), rel, "support"))
    monkeypatch.setattr(dp, "TARGET_PRESETS", {"game": {"target_dir": str(target)}})
    publish(items, "editor")
    assert (target / "mc2.exe").read_bytes() == b"game bytes"
    hashes = publish(game_items, "game")
    for rel, row in rows.items():
        assert (target / rel).read_bytes() == b"editor bytes"
        assert hashes[rel][:2] == row
    if drop_editor_file:
        hashes = publish([], "editor")
        for rel in rows:
            assert rel not in hashes and not (target / rel).exists()


@pytest.fixture(scope='module')
def hardlink_support(tmp_path_factory):
    tmp_path = tmp_path_factory.mktemp('hardlink-probe')
    source = tmp_path / 'source'
    source.write_bytes(b'probe')
    try:
        os.link(source, tmp_path / 'link')
    except OSError as exc:
        # Win32: INVALID_FUNCTION, NOT_SAME_DEVICE, NOT_SUPPORTED,
        # PRIVILEGE_NOT_HELD (1, 17, 50, 1314 respectively).
        if exc.errno in {errno.EXDEV, errno.EPERM, errno.EOPNOTSUPP, errno.ENOTSUP} or getattr(exc, 'winerror', None) in {1, 17, 50, 1314}:
            pytest.skip(f'Hardlinks unsupported in test temporary directory: {exc}')
        raise


@pytest.mark.parametrize('error_code,winerror,unsupported', [
    (errno.EXDEV, None, True), (errno.EPERM, None, True),
    (errno.EOPNOTSUPP, None, True), (errno.ENOTSUP, None, True),
    (None, 1, True), (None, 17, True), (None, 50, True), (None, 1314, True),
    (errno.ENOSPC, 112, False), (errno.EIO, 1117, False), (errno.EACCES, 5, False),
])
def test_hardlink_probe_only_skips_unsupported(tmp_path_factory, monkeypatch, error_code, winerror, unsupported):
    error = OSError(error_code, 'hardlink probe failure')
    error.winerror = winerror

    def fail_link(*args):
        raise error

    monkeypatch.setattr(os, 'link', fail_link)
    with pytest.raises(pytest.skip.Exception if unsupported else OSError) as caught:
        hardlink_support.__wrapped__(tmp_path_factory)
    if not unsupported:
        assert caught.value is error


@pytest.mark.parametrize('receipt', [True, False])
def test_five_link_payload_detaches_only_with_lane_receipt(tmp_path, monkeypatch, receipt, hardlink_support):
    """Audit fringe/bc7 refusal: changed payload and manifest, five links each."""
    monkeypatch.setenv('MC2_COOK_CACHE', str(tmp_path / 'cook-cache'))
    monkeypatch.setenv('MC2_LEAN_SEED', str(tmp_path / 'absent-seed'))
    seed, lane = tmp_path / 'seed', tmp_path / 'lane'
    seed.mkdir()
    lane.mkdir()
    rels = ['mc2.exe', 'runtime.dll', dp.MANIFEST_NAME]
    for rel in rels:
        (seed / rel).write_bytes(b'seed bytes')
        os.link(seed / rel, lane / rel)
        for n in range(3):
            os.link(seed / rel, seed / f'{rel}.peer-{n}')
        assert (lane / rel).stat().st_nlink == 5
    if receipt:
        (lane / '.lane-seed.json').write_text(json.dumps({'shared': {}}))
    source = tmp_path / 'new-payload'
    source.write_bytes(b'new lane bytes')
    items = [(str(source), rel, 'support') for rel in rels[:-1]]
    writes = items + [(None, dp.MANIFEST_NAME, 'manifest')]
    verified = dp.prepare_seed_delta(writes, str(lane))
    assert verified == {}
    if not receipt:
        # Outside legacy eligibility, owner/box installs still refuse shared writes.
        with pytest.raises(SystemExit):
            dp.check_hardlink_isolation(writes, str(lane))
        assert all((lane / rel).stat().st_nlink == 5 for rel in rels)
    else:
        dp.check_hardlink_isolation(writes, str(lane))
        assert all((lane / rel).stat().st_nlink == 1 for rel in rels)
        monkeypatch.setattr(dp, 'ensure_terrain_materials', lambda *a: {})
        monkeypatch.setattr(dp, 'ensure_engine_normal_cooks', lambda *a: None)
        monkeypatch.setattr(dp, 'retire_unowned', lambda *a, **kw: None)
        hashes = dp.deploy(items, str(lane), False, verified=verified)
        dp.write_manifest(str(lane), hashes, 'lane-tip')
        assert all((lane / rel).read_bytes() == b'new lane bytes' for rel in rels[:-1])
        assert set(dp.read_manifest(str(lane))) == set(rels[:-1])
    for rel in rels:
        assert (seed / rel).read_bytes() == b'seed bytes'
        assert all((seed / f'{rel}.peer-{n}').read_bytes() == b'seed bytes' for n in range(3))
    assert not list(lane.glob('*.private-*'))


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
def test_run_deploy_logs_stock_receipts_and_kit_cook(tmp_path, monkeypatch, capsys, kit_enabled, target_name="lane"):
    import asset_parity
    import kit_payload
    from smoke_lib import render_options_pin
    build, target = tmp_path / "build", tmp_path / "target"
    build.mkdir()
    target.mkdir()
    (build / "mc2").write_bytes(b"exe")
    for name in ("seed_build_dlls", "check_exe_fresh", "check_free_space_for_payload", "check_hardlink_isolation",
                 "ensure_engine_normal_cooks", "cook_mod_visual_heights", "write_manifest"):
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
    registry = target / "data/model_overrides/models.json"
    registry.parent.mkdir(parents=True)
    registry.write_text("{}")
    monkeypatch.setattr(kit_payload, "finish_trees", lambda *a, **kw: (_ for _ in ()).throw(ValueError("bad tree")))
    # This synthetic deploy tests phase logging, independently of the host's stock assets.
    monkeypatch.setattr(asset_parity, "enforce", lambda *a, **kw: None)
    monkeypatch.setattr(dp.subprocess, "run", lambda *a, **kw: SimpleNamespace(stdout=""))
    args = SimpleNamespace(target=target_name, allow_new_target=True, allow_stale_exe=False, build64_root=None, classic_assets=False, skip_kit=False,
                           require_clean=False, full_verify=False, allow_stale_pdb=False, preserve=[], no_asset_sync=False)
    probes = ("mc2.private-99999999", ".deployed_manifest.csv.private-99999999", "owner.private",
              "mods/custom/model.private-12345", "savegame/owner.fit", "crashes/owner.dmp")
    for rel in probes:
        path = target / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"owner bytes")
    if target_name == "game":  # a failed precook fails the v0.4 deploy
        with pytest.raises(SystemExit):
            dp.run_deploy(args, str(tmp_path), str(build), str(target), "mc2", "mc2.pdb")
        assert "FAIL: tree model precook failed: bad tree" in capsys.readouterr().err
        return
    dp.run_deploy(args, str(tmp_path), str(build), str(target), "mc2", "mc2.pdb")
    assert all((target / rel).read_bytes() == b"owner bytes" for rel in probes)
    out = capsys.readouterr().out
    assert "WARN: tree model precook failed: bad tree" in out
    assert all(f"phase {phase} seconds=" in out for phase in ("hash/verify", "copy", "stock receipts", "kit cook"))
    assert phases == (["stock", "cook", "cook"] if kit_enabled else [])



def test_run_deploy_game_target_fails_when_tree_precook_fails(tmp_path, monkeypatch, capsys):
    test_run_deploy_logs_stock_receipts_and_kit_cook(tmp_path, monkeypatch, capsys, False, target_name="game")

def test_mod_height_cook_is_lazy_without_paks(tmp_path, monkeypatch):
    import builtins
    original = builtins.__import__
    def guarded(name, *args, **kwargs):
        assert name not in ("mission_terrain_analyzer", "height_sig", "numpy")
        return original(name, *args, **kwargs)
    monkeypatch.setattr(builtins, "__import__", guarded)
    before = list(sys.path)
    dp.cook_mod_visual_heights(ROOT, tmp_path)
    assert sys.path == before


def test_mod_height_skip_validates_size_signature_and_continues(tmp_path, monkeypatch):
    import types
    import numpy as np
    maps = tmp_path / "mods/test/data/missions"
    maps.mkdir(parents=True)
    for name in ("a", "b"):
        (maps / (name + ".pak")).touch()
    monkeypatch.setitem(sys.modules, "mission_terrain_analyzer", types.SimpleNamespace(
        read_packets=lambda p: [], locate_mapdata=lambda p: (0, 2, b""),
        extract_layers=lambda *a: {"elev": np.zeros((2, 2))}))
    monkeypatch.setitem(sys.modules, "height_sig", types.SimpleNamespace(height_sig=lambda e: "2 fixture"))
    calls = []
    def fail(cmd, **kwargs):
        calls.append(cmd[2])
        raise subprocess.CalledProcessError(8, cmd)
    monkeypatch.setattr(dp.subprocess, "run", fail)
    beauty = maps / "a.beauty"
    beauty.mkdir()
    height = beauty / "visual_height_4x.r32"
    sig = beauty / "gameplay_height.sig"
    height.write_bytes(bytes(100))
    sig.write_text("2 fixture")
    dp.cook_mod_visual_heights(ROOT, tmp_path)
    assert calls == ["b"]
    height.write_bytes(b"truncated")
    calls.clear()
    dp.cook_mod_visual_heights(ROOT, tmp_path)
    assert calls == ["a", "b"]
    height.write_bytes(bytes(100))
    sig.write_text("2 stale")
    calls.clear()
    dp.cook_mod_visual_heights(ROOT, tmp_path)
    assert calls == ["a", "b"]

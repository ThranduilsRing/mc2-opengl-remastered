"""deploy_payload: a fresh Ninja tree is seeded with its runtime DLLs from real sources; --target names the right flag."""
import subprocess
import sys
import pytest
from pathlib import Path

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

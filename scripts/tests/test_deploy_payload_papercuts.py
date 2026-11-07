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

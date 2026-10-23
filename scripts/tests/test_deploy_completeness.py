"""check_install_complete: missing runtime DLLs are copied from the build dir, missing assets/data/shaders are reported."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402

EXE = "mc2.exe"


def test_completeness(tmp_path):
    build, target = tmp_path / "build", tmp_path / "target"
    build.mkdir()
    target.mkdir()
    (target / EXE).write_bytes(b"MZ")
    for d in dp.FFMPEG_DLLS:
        (build / d).write_bytes(b"x")
    # check-only: nothing copied, everything missing is listed
    gaps = dp.check_install_complete(str(target), EXE, str(build), fix=False)
    assert set(dp.FFMPEG_DLLS) <= set(gaps) and {"assets/", "data/", "shaders/"} <= set(gaps)
    assert not (target / dp.FFMPEG_DLLS[0]).exists()
    # fix: DLLs copied from the build dir, only the folders remain
    assert dp.check_install_complete(str(target), EXE, str(build)) == ["assets/", "data/", "shaders/"]
    assert (target / dp.FFMPEG_DLLS[0]).is_file()
    for d in dp.REQUIRED_DIRS:
        (target / d).mkdir()
        (target / d / "f").write_bytes(b"x")
    assert dp.check_install_complete(str(target), EXE, str(build), fix=False) == []

"""A code-only deploy still verifies the deploy-owned engine normals (asset_parity no longer does)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy_payload as dp  # noqa: E402


def make_install(tmp_path, version=None):
    tex = tmp_path / "data" / "textures"
    tex.mkdir(parents=True)
    for i in range(6):
        (tex / f"mat{i}_normal.ktx2").write_bytes(b"x")
    (tex / "engine_normals.cookver").write_text((version or dp.ENGINE_NORMAL_COOK_VERSION) + "\n")
    return tmp_path


def test_all_present_passes(tmp_path):
    dp.verify_engine_normals_present(str(make_install(tmp_path)))


def test_missing_file_fails_naming_it(tmp_path, capsys):
    install = make_install(tmp_path)
    (install / "data" / "textures" / "mat3_normal.ktx2").unlink()
    with pytest.raises(SystemExit):
        dp.verify_engine_normals_present(str(install))
    assert "mat3_normal.ktx2 missing" in capsys.readouterr().err


def test_stale_cookver_fails(tmp_path, capsys):
    install = make_install(tmp_path, version="0")
    with pytest.raises(SystemExit):
        dp.verify_engine_normals_present(str(install))
    assert "engine_normals.cookver" in capsys.readouterr().err

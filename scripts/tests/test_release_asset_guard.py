"""Synthetic fixtures only: no installed BattleTech assets are read or copied."""
from pathlib import Path
import io
import struct
import subprocess
import sys
import zipfile

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_asset_guard as guard
import test_release_zip_install as release

REPO = Path(__file__).resolve().parents[2]
FORBIDDEN_NAMES = [
    "mods/bt2018_mechs/atlas/mesh.bin", "mods/BT2018Mechs/mod.json",
    "data/tgl/bt2018/texture.ktx2", "chrTxrMech_fake.tga", "CHRPRFMECH_fake.bin",
    "chrMatMech_fake.json", "uixTxr_blip_fake.png", "uixTxrIcon_fake.png",
    "bt2018_fake_hud_icon.tga", "bt2018_fake_bay.tga", "fake_fbx2gltf.glb",
    "data/tgl/Flea.glb", "data/tgl/Flea.package.json", "model.FBX",
    "fake.assets", "fake.resS", "VersionManifest.csv", "chassisdef_fake.json",
    "mechdef_fake.json", "hardpointdatadef_fake.json", "movedef_fake.json",
    "Weapon_fake.json", "AssetStudio.exe", "FBX2glTF.exe", "libfbxsdk.dll",
    "MC2Conversions/file.bin", "BattleTech_2018_Dump/file.bin",
    "tools/bt2018_kit/_bin/tool.exe", "tools/bt2018_kit/work/file.bin",
    "tools/bt2018_kit/_bin/roster.json",
]


def fake_glb():
    payload = b'{"nodes":[{"name":"j_COCKPIT"},{"name":"atlas_moveCore"}]}'
    payload += b" " * (-len(payload) % 4)
    return struct.pack("<4sIII4s", b"glTF", 2, 20 + len(payload), len(payload), b"JSON") + payload


def assert_rejected(root, name, payload=b"synthetic fixture"):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    assert guard.scan(root), f"guard accepted {name}"


@pytest.mark.parametrize("name", FORBIDDEN_NAMES)
def test_forbidden_names(tmp_path, name):
    assert_rejected(tmp_path, name)


@pytest.mark.parametrize("payload", [
    b"UnityFS\0fixture", b"UnityWeb\0fixture", b"UnityRaw\0fixture",
    b"Kaydara FBX Binary\0fixture", b"prefix chrTxrMech_fake suffix",
    b"chrPrfMech_fake", b"chrMatMech_fake", fake_glb(),
])
def test_renamed_content(tmp_path, payload):
    assert_rejected(tmp_path, "innocent.bin", payload)


@pytest.mark.parametrize("token", guard.TOKENS + guard.NODES)
def test_chunk_boundary(tmp_path, token):
    prefix = b"atlas_moveCore " if token == b"j_COCKPIT" else b"j_COCKPIT "
    payload = prefix + b"x" * (guard.CHUNK_SIZE - len(prefix) - 3) + token
    assert_rejected(tmp_path, "boundary.bin", payload)


def test_kit_code_only_passes(tmp_path):
    for name in ("tools/bt2018_kit/run.py", "tools/bt2018_mechs/roster.json",
                 "tools/bt2018_mechs/data/weapon_map_v1.json",
                 "tools/bt2018_import/run.bat", "scripts/bt2018_retarget_clips.py",
                 "docs/kit.md", "scripts/tests/test_fixture.py"):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("chrTxrMech_fake chrPrfMech_fake chrMatMech_fake", encoding="utf-8")
    assert guard.scan(tmp_path) == []


@pytest.mark.parametrize("payload", [b"\0chrTxrMech_fake", b"\xffchrTxrMech_fake",
                                    b"UnityFS\0", fake_glb(), b"chrTxrMech_fake\xc3"])
def test_binary_cannot_use_text_exception(tmp_path, payload):
    assert_rejected(tmp_path, "tools/bt2018_kit/roster.json", payload)


@pytest.mark.parametrize("name,payload", [("data/tgl/texture.ktx2", b"chrMatMech_fake"),
                                         ("mods\\BT2018Mechs\\mod.json", b"{}"),
                                         ("./mods/bt2018_mechs/mod.json", b"{}"),
                                         ("renamed.bin", fake_glb())])
def test_zip_members(tmp_path, name, payload):
    archive = tmp_path / "package.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr(name, payload)
    assert guard.scan(archive)


def test_clean_zip_and_tree(tmp_path):
    (tmp_path / "model.glb").write_bytes(b"authored fixture")
    assert guard.scan(tmp_path) == []
    archive = tmp_path / "package.zip"
    with zipfile.ZipFile(archive, "w") as out:
        out.writestr("tools/bt2018_kit/recipe.json", b'"chrTxrMech_fake"')
    assert guard.scan(archive) == []


def test_unreadable_and_unsafe_inputs_fail_closed(tmp_path):
    assert guard.scan(tmp_path / "missing.zip")
    archive = tmp_path / "broken.zip"
    archive.write_bytes(b"not a zip")
    assert guard.scan(archive)
    with zipfile.ZipFile(archive, "w") as out:
        out.writestr("../tools/bt2018_kit/recipe.json", b"chrMatMech_fake")
    assert guard.scan(archive)


def test_cli_and_verify_tree(tmp_path):
    assert_rejected(tmp_path, "renamed.bin", fake_glb())
    assert any("renamed.bin" in item for item in release.verify_tree(tmp_path))
    result = subprocess.run([sys.executable, "-B", str(REPO / "scripts/release_asset_guard.py"),
                             str(tmp_path)], capture_output=True, text=True)
    assert result.returncode == 1
    assert "renamed.bin:" in result.stdout


def test_reads_are_bounded():
    class BoundedStream(io.BytesIO):
        def read(self, size=-1):
            assert size == 1024 * 1024
            return super().read(size)
    assert guard._inspect("ordinary.bin", BoundedStream(b"clean")) == []


def test_negative_control(tmp_path, monkeypatch):
    # Exercise the same assertion used by every forbidden fixture with a disabled guard.
    monkeypatch.setattr(guard, "scan", lambda path: [])
    with pytest.raises(AssertionError, match="guard accepted"):
        assert_rejected(tmp_path, "renamed.bin", fake_glb())


def test_tracked_repository_is_clean():
    # Scan tracked paths in place: no copying real assets into a fixture/artifact.
    result = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, check=True, capture_output=True)
    violations = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        name = raw.decode("utf-8")
        path = REPO / name
        if path.is_symlink():
            violations.append(f"{name}: symlink cannot be reviewed")
        else:
            with path.open("rb") as stream:
                violations.extend(guard._inspect(name, stream))
    assert not violations, "Tracked asset hits (do not delete or binary-allowlist):\n" + "\n".join(violations)

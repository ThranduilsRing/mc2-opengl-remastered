"""Synthetic foreign fixtures only; MC2 PacketFile fixtures are inspected in place."""
from pathlib import Path
import io
import struct
import subprocess
import sys
import zipfile

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import console_child
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
    "mods/mw5_mechs/mech/model.glb", "mods/MW5Other/mod.json",
    "data/tgl/mw5_fake.tga", "data/tgl/128/mw5_fake.ktx2",
    "nested/UmodelExport/roster.json", "mw5_extract/file.bin", "nested/mw5-ref/file.bin",
    *("fake." + ext for ext in ("pak", "utoc", "ucas", "uasset", "uexp", "ubulk", "umap", "assetbundle", "uptnl",
                               "usmap", "psk", "pskx", "psa", "uemodel", "ueanim")),
    "Umodel.exe", "FModel.exe", "CUE4Parse.dll", "oo2core_9_win64.dll", "Oodle.dll",
    "Weapon_Mech_fake.glb", "fake_SKM.glb", "fake_MSK.tga", "fake_MTI.json",
    "Timberwolf_fake.glb", "TBR_fake.glb",
    "tools/mw5_mechs/work/recipe.json", "tools/mw5_kit/_bin/tool.exe",
]


def fake_glb(payload=b'{"nodes":[{"name":"j_COCKPIT"},{"name":"atlas_moveCore"}]}'):
    payload += b" " * (-len(payload) % 4)
    return struct.pack("<4sIII4s", b"glTF", 2, 20 + len(payload), len(payload), b"JSON") + payload


def assert_rejected(root, name, payload=b"synthetic fixture"):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    assert guard.scan(root), f"guard accepted {name}"


@pytest.mark.parametrize("name", FORBIDDEN_NAMES, ids=[f"name{i}" for i in range(len(FORBIDDEN_NAMES))])
def test_forbidden_names(tmp_path, name):
    assert_rejected(tmp_path, name)


@pytest.mark.parametrize("payload", [
    b"UnityFS\0fixture", b"UnityWeb\0fixture", b"UnityRaw\0fixture",
    b"Kaydara FBX Binary\0fixture", b"prefix chrTxrMech_fake suffix",
    b"chrPrfMech_fake", b"chrMatMech_fake", fake_glb(),
], ids=["unity-fs", "unity-web", "unity-raw", "fbx", "texture", "profile", "material", "glb"])
def test_renamed_content(tmp_path, payload):
    assert_rejected(tmp_path, "innocent.bin", payload)


@pytest.mark.parametrize("payload", [
    b"synthetic pak" + struct.pack("<I", 0x5A6F12E1) + b"x" * 252,
    b"prefix" + b"-==-" * 4 + b"suffix", struct.pack("<I", 0x9E2A83C1),
    b"ACTR" + b"HEAD", b"ANIM" + b"HEAD",
    fake_glb(b'{"asset":{"generator":"tools/mw5_mechs"}}'),
    *(fake_glb(b'{"nodes":[{"name":"Torso_Twist"},{"name":"' + node + b'"}]}')
      for node in (b"Foot0_L", b"WeldTarget_R", b"Torso_DM0")),
], ids=["pak", "iostore", "unreal", "actor", "anim", "generator", "foot", "weld", "damage"])
@pytest.mark.parametrize("name", ["renamed.bin", "tools/mw5_kit/recipe.json"], ids=["binary", "recipe"])
def test_mw5_renamed_content(tmp_path, name, payload):
    # Node-only GLBs intentionally contain no animation clips.
    assert_rejected(tmp_path, name, payload)


@pytest.mark.parametrize("payload", [
    b"x" * (guard.CHUNK_SIZE - 3) + b"-==-" * 4,
    fake_glb(b'{"padding":"' + b"x" * (guard.CHUNK_SIZE - 40) +
             b'","asset":{"generator":"tools/mw5_mechs"}}'),
], ids=["iostore", "glb"])
def test_mw5_chunk_boundaries(tmp_path, payload):
    assert_rejected(tmp_path, "renamed.bin", payload)


def test_mw5_content_scope():
    for payload in (struct.pack("<I", 0x5A6F12E1) + b"x" * 256,
                    b"prefix" + struct.pack("<I", 0x9E2A83C1),
                    fake_glb(b'{"nodes":[{"name":"Torso_Twist"}]}'),
                    fake_glb(b'{}') + b"Torso_Twist Foot0_L tools/mw5_mechs"):
        assert guard._inspect("authored.bin", io.BytesIO(payload)) == []


@pytest.mark.parametrize("token", guard.TOKENS + guard.NODES, ids=["texture", "profile", "material", "cockpit", "core"])
def test_chunk_boundary(tmp_path, token):
    prefix = b"atlas_moveCore " if token == b"j_COCKPIT" else b"j_COCKPIT "
    payload = prefix + b"x" * (guard.CHUNK_SIZE - len(prefix) - 3) + token
    assert_rejected(tmp_path, "boundary.bin", payload)


def test_kit_code_only_passes(tmp_path):
    for name in ("tools/bt2018_kit/run.py", "tools/bt2018_mechs/roster.json",
                 "tools/bt2018_mechs/data/weapon_map_v1.json",
                 "tools/bt2018_import/run.bat", "scripts/bt2018_retarget_clips.py",
                 "tools/mw5_mechs/weapon_mech_map.json", "tools/mw5_kit/tbr_recipe.py",
                 "docs/kit.md", "scripts/tests/test_fixture.py"):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("chrTxrMech_fake chrPrfMech_fake chrMatMech_fake", encoding="utf-8")
    assert guard.scan(tmp_path) == []


@pytest.mark.parametrize("payload", [b"\0chrTxrMech_fake", b"\xffchrTxrMech_fake",
                                    b"UnityFS\0", fake_glb(), b"chrTxrMech_fake\xc3"],
                         ids=["nul", "invalid-utf8", "unity", "glb", "truncated-utf8"])
def test_binary_cannot_use_text_exception(tmp_path, payload):
    assert_rejected(tmp_path, "tools/bt2018_kit/roster.json", payload)


@pytest.mark.parametrize("name,payload", [("data/tgl/texture.ktx2", b"chrMatMech_fake"),
                                         ("mods\\BT2018Mechs\\mod.json", b"{}"),
                                         ("./mods/bt2018_mechs/mod.json", b"{}"),
                                         ("renamed.bin", fake_glb())], ids=["texture", "backslash", "dot", "glb"])
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


@pytest.mark.parametrize("magic", [b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08",
                                   b"7z\xbc\xaf\x27\x1c", b"Rar!\x1a\x07\x00",
                                   b"Rar!\x1a\x07\x01\x00", b"\x1f\x8b"],
                         ids=["zip", "zip-empty", "zip-spanned", "7z", "rar4", "rar5", "gzip"])
def test_archive_magic_any_extension(tmp_path, magic):
    name = "docs/innocent.txt"
    assert guard._inspect(name, io.BytesIO(magic), reject_archives=True) == [
        f"{name}: nested archive not allowed in release payload"]
    archive = tmp_path / "package.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr(name, magic + b"synthetic fixture")
    assert guard.scan(archive) == [f"{name}: nested archive not allowed in release payload"]


def test_nested_zip_is_rejected_without_extraction(tmp_path):
    inner = io.BytesIO()
    with zipfile.ZipFile(inner, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("renamed.bin", fake_glb())
    archive = tmp_path / "package.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("innocent.bin", inner.getvalue())
    assert guard.scan(archive) == ["innocent.bin: nested archive not allowed in release payload"]
    (tmp_path / "renamed.dat").write_bytes(inner.getvalue())
    assert "renamed.dat: nested archive not allowed in release payload" in guard.scan(tmp_path)


@pytest.mark.parametrize("name", ["tests/unit/fixtures/stock_names.txt", "tools/mech_import/build_marauder.py"],
                         ids=["stock", "marauder"])
def test_exact_legacy_text_exceptions(name):
    payload = b"chrTxrMech_fake"
    assert guard._inspect(name, io.BytesIO(payload)) == []
    assert guard._inspect("other/" + name, io.BytesIO(payload))
    assert guard._inspect(name, io.BytesIO(b"\0" + payload))


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
                             str(tmp_path)], capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 1
    assert "renamed.bin:" in result.stdout


def test_reads_are_bounded():
    class BoundedStream(io.BytesIO):
        def read(self, size=-1):
            assert size == 1024 * 1024
            return super().read(size)
    assert guard._inspect("ordinary.bin", BoundedStream(b"clean")) == []


@pytest.mark.parametrize("payload", [fake_glb(), fake_glb(
    b'{"nodes":[{"name":"Torso_Twist"},{"name":"Foot0_L"}]}')], ids=["bt", "mw5"])
def test_negative_control(tmp_path, monkeypatch, payload):
    # Exercise the same assertion used by every forbidden fixture with a disabled guard.
    monkeypatch.setattr(guard, "scan", lambda path: [])
    with pytest.raises(AssertionError, match="guard accepted"):
        assert_rejected(tmp_path, "renamed.bin", payload)


def test_mc2_packet_fixture_passes():
    path = REPO / "editor/tests/goldens/e5a-stock/e5_reference_before.pak"
    with path.open("rb") as stream:
        assert guard._inspect(path.name, stream) == []


@pytest.mark.parametrize("payload", [b"", b"\xce\xfa\xed", b"\xfe\xed\xfa\xce",
    b"synthetic UE pak" + struct.pack("<I", 0x5A6F12E1)], ids=["empty", "short", "endian", "ue"])
def test_foreign_pak_rejected(tmp_path, payload):
    assert_rejected(tmp_path, "foreign.PAK", payload)


@pytest.mark.parametrize("payload", [
    br'{"asset":{"generator":"tools\/mw5_mechs"}}',
    br'{"asset":{"generator":"\u0074ools/mw5_mechs"}}',
    br'{"nodes":[{"name":"\u0054orso_Twist"},{"name":"\u0046oot0_L"}]}',
    b'{"padding":"' + b"x" * (guard.CHUNK_SIZE - 22 - len(b'{"padding":"","asset":{"generator":"')) +
    br'","asset":{"generator":"\u0074ools\/mw5_mechs"}}',
], ids=["slash", "generator-unicode", "nodes-unicode", "split-escape"])
def test_glb_json_escapes(tmp_path, payload):
    assert_rejected(tmp_path, "renamed.bin", fake_glb(payload))


def test_tracked_repository_is_clean():
    # Scan tracked paths in place: no copying real assets into a fixture/artifact.
    result = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, check=True, capture_output=True,
                            **console_child.child_kwargs(redirected=True))
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

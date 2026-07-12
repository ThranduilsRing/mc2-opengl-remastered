"""tex_hd.py tests — .ase/.ini texture-ref extraction, TGA/KTX2 header parsing,
dry-run plan, and arg-parsing. No upscaler/ktx.exe required."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tex_hd as th  # noqa: E402

ASE_TEXT = (
    '*MATERIAL_LIST {\n'
    '  *MATERIAL 0 {\n'
    '    *BITMAP "\\\\Aas1\\mc2\\Art\\Buildings\\A_Quonset.tga"\n'
    '  }\n'
    '}\n'
)

INI_TEXT = (
    'FITini\n\n[TGLData]\n'
    'st FileName0="Quonset"\n'
    'st FileName1="QuonsetRoof"\n'
    'st ShadowName="QuonsetX"\n\n'
    'FITend\n'
)


# ---------------------------------------------------------------------------
# Pure parsing helpers
# ---------------------------------------------------------------------------

def test_parse_ase_bitmaps_dedups_and_strips_path():
    assert th.parse_ase_bitmaps(ASE_TEXT) == ["A_Quonset.tga"]


def test_parse_ase_bitmaps_empty_on_no_match():
    assert th.parse_ase_bitmaps("*GEOMOBJECT {\n}\n") == []


def test_parse_ini_shape_stems_order_and_dedup():
    text = INI_TEXT + 'st FileName0="Quonset"\n'  # duplicate, case-same
    assert th.parse_ini_shape_stems(text) == ["Quonset", "QuonsetRoof", "QuonsetX"]


def test_find_case_insensitive(tmp_path):
    (tmp_path / "A_Quonset.tga").write_bytes(b"x")
    hit = th.find_case_insensitive(tmp_path, "a_quonset.tga")
    assert hit is not None
    assert hit.name.lower() == "a_quonset.tga"
    assert th.find_case_insensitive(tmp_path, "missing.tga") is None
    assert th.find_case_insensitive(tmp_path / "nope", "a_quonset.tga") is None


def _make_tga(path: Path, w: int, h: int):
    hdr = bytearray(18)
    struct.pack_into("<HH", hdr, 12, w, h)
    path.write_bytes(bytes(hdr))


def test_tga_dims(tmp_path):
    p = tmp_path / "a.tga"
    _make_tga(p, 128, 256)
    assert th.tga_dims(p) == (128, 256)


def test_tga_dims_short_file_returns_none(tmp_path):
    p = tmp_path / "short.tga"
    p.write_bytes(b"\x00\x01")
    assert th.tga_dims(p) is None


def _make_ktx2(path: Path, w: int, h: int):
    hdr = bytearray(28)
    hdr[0:12] = th._KTX2_MAGIC
    struct.pack_into("<II", hdr, 20, w, h)
    path.write_bytes(bytes(hdr))


def test_ktx2_dims(tmp_path):
    p = tmp_path / "a.ktx2"
    _make_ktx2(p, 512, 512)
    assert th.ktx2_dims(p) == (512, 512)


def test_ktx2_dims_bad_magic_returns_none(tmp_path):
    p = tmp_path / "bad.ktx2"
    p.write_bytes(b"\x00" * 28)
    assert th.ktx2_dims(p) is None


def test_build_cook_cmd_shape():
    cmd = th.build_cook_cmd(Path("src.tga"), Path("dst.ktx2"), 4, python="py3")
    assert cmd[0] == "py3"
    assert str(th.COOK_SINGLE) in cmd
    assert "--src" in cmd and "src.tga" in cmd
    assert "--scale" in cmd and "4" in cmd


# ---------------------------------------------------------------------------
# Asset -> texture-ref resolution (fixture srcdata root)
# ---------------------------------------------------------------------------

@pytest.fixture
def ase_root(tmp_path):
    root = tmp_path / "tgl"
    root.mkdir()
    (root / "quonset.ase").write_text(ASE_TEXT)
    (root / "quonset.ini").write_text(INI_TEXT)
    # shapes referenced by the ini but with their own .ase (dupe bitmap by name)
    (root / "QuonsetRoof.ase").write_text(ASE_TEXT)
    return root


def test_collect_texture_refs_via_ini_and_ase(ase_root):
    refs, notes = th.collect_texture_refs("quonset", ase_root)
    assert refs == ["A_Quonset.tga"]
    assert any("quonset.ini" in n for n in notes)


def test_collect_texture_refs_missing_ini_falls_back_to_bare_ase(tmp_path):
    root = tmp_path / "tgl"
    root.mkdir()
    (root / "solo.ase").write_text(ASE_TEXT)
    refs, notes = th.collect_texture_refs("solo", root)
    assert refs == ["A_Quonset.tga"]
    assert any("no solo.ini" in n for n in notes)


def test_collect_texture_refs_no_ase_reports_note(tmp_path):
    root = tmp_path / "tgl"
    root.mkdir()
    refs, notes = th.collect_texture_refs("ghost", root)
    assert refs == []
    assert any("no .ase found" in n for n in notes)


def test_locate_source_tga_searches_roots_in_order(tmp_path):
    root_a = tmp_path / "a"
    root_b = tmp_path / "b"
    root_a.mkdir()
    root_b.mkdir()
    (root_b / "A_Quonset.tga").write_bytes(b"x")
    hit = th.locate_source_tga("A_Quonset.tga", [root_a, root_b])
    assert hit == root_b / "A_Quonset.tga"


def test_locate_source_tga_returns_none_when_absent(tmp_path):
    root = tmp_path / "a"
    root.mkdir()
    assert th.locate_source_tga("nope.tga", [root]) is None


# ---------------------------------------------------------------------------
# Deploy seam
# ---------------------------------------------------------------------------

def test_deploy_targets_prefers_sidecar_when_tga_present(tmp_path):
    deploy = tmp_path / "install"
    tga_dir = deploy / "data" / "tgl" / "128"
    tga_dir.mkdir(parents=True)
    (tga_dir / "a_quonset.tga").write_bytes(b"x")
    target, seam = th.deploy_targets(deploy, "a_quonset", 512)
    assert seam == "sidecar"
    assert target == tga_dir / "a_quonset.ktx2"


def test_deploy_targets_falls_back_to_tier_dir(tmp_path):
    deploy = tmp_path / "install"
    target, seam = th.deploy_targets(deploy, "a_quonset", 512)
    assert seam == "tier"
    assert target == deploy / "data" / "tgl" / "512" / "a_quonset.ktx2"


# ---------------------------------------------------------------------------
# CLI dry-run (no subprocess/upscaler required)
# ---------------------------------------------------------------------------

def test_main_dry_run_reports_plan(ase_root, capsys):
    # No source .tga in the fixture -> dry-run still resolves refs but every
    # texture MISSes on locate_source_tga (rc=1); that's the correct signal.
    rc = th.main(["quonset", "--ase-root", str(ase_root), "--dry-run"])
    assert rc == 1
    out = capsys.readouterr()
    assert "quonset" in out.out
    assert "no source .tga" in out.err


def test_main_dry_run_with_source_tga_shows_plan(ase_root, capsys):
    (ase_root / "A_Quonset.tga").write_bytes(bytes(18))  # 0x0 header, fine for dry-run
    rc = th.main(["quonset", "--ase-root", str(ase_root), "--dry-run",
                  "--tga-root", str(ase_root)])
    assert rc == 0
    out = capsys.readouterr().out
    assert "PLAN" in out


def test_main_dry_run_no_refs_is_a_failure(tmp_path, capsys):
    root = tmp_path / "tgl"
    root.mkdir()
    rc = th.main(["ghost", "--ase-root", str(root), "--dry-run"])
    assert rc == 1


def test_main_rejects_bad_scale(ase_root, capsys):
    rc = th.main(["quonset", "--ase-root", str(ase_root), "--scale", "0"])
    assert rc == 2


def test_main_rejects_missing_ase_root(tmp_path, capsys):
    rc = th.main(["quonset", "--ase-root", str(tmp_path / "nope")])
    assert rc == 2

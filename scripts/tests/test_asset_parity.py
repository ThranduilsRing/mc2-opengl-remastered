"""ASSET-PARITY-1: lane installs carry v0.4's data/, assets/ and *.fst, and captures refuse drift."""
import itertools
import os
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import asset_parity as ap  # noqa: E402

_COUNTER = itertools.count()


@pytest.fixture
def work():
    """A plain scratch dir (work's mkdtemp mode 0700 is unenumerable under the lane sandbox)."""
    base = Path(os.environ.get("MC2_ASSET_PARITY_TEST_ROOT") or tempfile.gettempdir())
    d = base / f"asset-parity-{os.getpid()}-{next(_COUNTER)}"
    d.mkdir(parents=True, exist_ok=True)
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)

SHARED = {
    "data/textures/1.2.txm": b"transition",
    "data/textures/256/cement_1.tga": b"tga",
    "data/art/mechicons/anubis.png": b"png",
    "assets/graphics/agencyfb11.bmp": b"bmp",
    "textures.fst": b"fst-archive",
    "tgl.fst": b"tgl-archive",
}
EXCLUDED = {
    "data/textures/_txm_cache_bak/1.2.txm": b"bak",
    "data/textures/txm_backup/1.txm": b"bak",
}
NOT_PARITY = {
    "mc2.exe": b"exe", "shaders/vulkan/a.spv": b"spv", "mods/x/mod.json": b"m",
    "cache/content-model/a.mci": b"c", "options.cfg": b"cfg",
}


def tree(root, files):
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return root


@pytest.fixture
def ref(work):
    return tree(work / "v04", {**SHARED, **EXCLUDED, **NOT_PARITY})


def test_index_covers_data_assets_and_root_fst_but_no_backups(ref):
    index = ap.reference_index(ref)
    assert set(index) == {rel.casefold() for rel in SHARED}
    assert ap.is_excluded("DATA\\Textures\\_txm_cache_bak\\1.2.txm")
    assert not ap.is_excluded("data/textures/1.2.txm")


def test_check_names_missing_and_size_differences(ref, work):
    inst = tree(work / "lane", SHARED)
    assert ap.compare(inst, ref).ok
    (inst / "data/textures/1.2.txm").unlink()
    (inst / "textures.fst").write_bytes(b"fst-archivE")          # same size, other byte (sampled)
    (inst / "assets/graphics/agencyfb11.bmp").write_bytes(b"bmp-but-longer")   # size differs
    result = ap.compare(inst, ref)
    assert result.missing == ["data/textures/1.2.txm"]
    assert result.size == ["assets/graphics/agencyfb11.bmp"]
    assert result.content == ["textures.fst"]
    assert result.total == len(SHARED)
    line = ap.verdict_line(result, ref)
    assert line.startswith("[ASSET_PARITY] FAIL ") and "missing=1" in line and "size=1" in line and "content=1" in line


def test_sync_copies_only_what_differs_and_replaces_shared_inodes(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / "data/textures/1.2.txm").unlink()
    (inst / "tgl.fst").write_bytes(b"old")
    before = (ref / "data/textures/1.2.txm").stat().st_mtime_ns
    result = ap.enforce(inst, ref, sync_missing=True)
    assert result.ok and result.synced == 2
    assert (inst / "data/textures/1.2.txm").read_bytes() == b"transition"
    assert (inst / "tgl.fst").read_bytes() == b"tgl-archive"
    assert (ref / "data/textures/1.2.txm").stat().st_mtime_ns == before      # reference untouched
    assert ap.verdict_line(result, ref).startswith("[ASSET_PARITY] SYNCED ")
    assert ap.compare(inst, ref).ok


def test_no_sync_leaves_the_install_alone(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / "textures.fst").unlink()
    result = ap.enforce(inst, ref, sync_missing=False)
    assert not result.ok and result.missing == ["textures.fst"]
    assert not (inst / "textures.fst").exists()


def test_reference_and_golden_are_never_targets(ref, work):
    assert ap.is_exempt(ref, ref)
    golden = work / "mc2-win64-v0.4c"
    golden.mkdir()
    assert ap.is_exempt(golden, ref)
    assert ap.enforce(ref, ref) is None
    assert ap.enforce(golden, ref) is None
    assert ap.capture_drift(ref, ref) is None


def test_no_reference_on_this_host_is_a_skip_not_a_failure(ref, work):
    missing_ref = work / "no-v04"
    assert ap.enforce(tree(work / "lane", SHARED), missing_ref) is None
    assert ap.capture_drift(tree(work / "lane2", SHARED), missing_ref) is None


def test_payload_owned_and_per_install_files_are_not_synced_from_the_reference(work):
    ref = tree(work / "v04", {"data/defs/ui/page.fit": b"v04-page", "data/tgl/x.ini": b"v04",
                                  "data/tgl/x.ini.cookstamp": b"1"})
    inst = tree(work / "inst", {"data/defs/ui/page.fit": b"lane-page", "data/tgl/x.ini": b"lane",
                                    "data/tgl/x.ini.cookstamp": b"2"})
    result = ap.enforce(inst, ref, skip={"data/defs/ui/page.fit", "data/tgl/x.ini"})
    assert result.ok and result.synced == 0 and result.total == 0
    assert (inst / "data/defs/ui/page.fit").read_bytes() == b"lane-page"
    assert (inst / "data/tgl/x.ini").read_bytes() == b"lane"
    # A file the deploy does NOT ship is still synced from the reference.
    (inst / "data/tgl/new.ini").unlink(missing_ok=True)
    ref2 = tree(work / "v04b", {"data/tgl/y.ini": b"v04-y"})
    assert ap.enforce(inst, ref2).synced == 1
    assert (inst / "data/tgl/y.ini").read_bytes() == b"v04-y"


def test_sample_is_deterministic_and_spread_over_directories(work):
    files = {f"data/textures/{i:04d}.tga": b"x" for i in range(50)}
    ref = tree(work / "v04", files)
    rels = [rel for rel, _ in ap.reference_index(ref).values()]
    picks = ap.sample_rels(rels, 10)
    assert picks == ap.sample_rels(rels, 10) and len(picks) == 10
    assert len({rel.split("/")[2] for rel in picks[:5]}) > 1
    assert ap.sample_rels(rels, 100) == sorted(rels)


def test_cli_check_reports_counts_and_never_copies(ref, work, capsys):
    inst = tree(work / "lane", SHARED)
    (inst / "tgl.fst").unlink()
    assert ap.main([str(inst), "--check", "--ref", str(ref)]) == 1
    out = capsys.readouterr().out
    assert "[ASSET_PARITY] FAIL" in out and "missing=1" in out
    assert not (inst / "tgl.fst").exists()
    assert ap.main([str(inst), "--sync", "--ref", str(ref)]) == 0
    assert (inst / "tgl.fst").read_bytes() == b"tgl-archive"


def test_capture_drift_reports_but_does_not_repair(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / "assets/graphics/agencyfb11.bmp").write_bytes(b"different")
    drift = ap.capture_drift(inst, ref)
    assert not drift.ok and drift.size == ["assets/graphics/agencyfb11.bmp"]
    assert (inst / "assets/graphics/agencyfb11.bmp").read_bytes() == b"different"


def test_capture_tools_refuse_drift_unless_allowed():
    """run_visual_capture and visual_verify/capture.py both parse --allow-asset-drift and call capture_drift."""
    src = (ROOT / "scripts/run_visual_capture.py").read_text(encoding="utf-8")
    assert '"--allow-asset-drift"' in src and "asset_parity.capture_drift(exe.parent)" in src
    cap = (ROOT / "tools/visual_verify/capture.py").read_text(encoding="utf-8")
    assert '"--allow-asset-drift"' in cap and "asset_parity.capture_drift(install)" in cap
    ab = (ROOT / "tools/visual_verify/ab.py").read_text(encoding="utf-8")
    assert "--allow-asset-drift" in ab and "allow_asset_drift=a.allow_asset_drift" in ab


def test_deploy_calls_parity_with_sync_and_no_sync_fails():
    src = (ROOT / "scripts/deploy_payload.py").read_text(encoding="utf-8")
    assert "--no-asset-sync" in src
    assert "asset_parity.enforce(target, sync_missing=not args.no_asset_sync" in src
    assert "asset_parity.PREFIX" in src
    smoke = (ROOT / "scripts/run_smoke.py").read_text(encoding="utf-8")
    assert "asset_parity.capture_drift" in smoke and "verdict_line" in smoke

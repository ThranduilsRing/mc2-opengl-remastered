"""Unit tests for tools/mod_smoke.py: staging, mission selection, health parsing."""
import importlib.util
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
_spec = importlib.util.spec_from_file_location("mod_smoke", ROOT / "tools" / "mod_smoke.py")
ms = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ms)


def _w(path: Path, text: str = "x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="latin-1")
    return path


def _campaign(first: str, second: str = "") -> str:
    out = '[Campaign]\nl GroupCount = 2\n[Group0Mission0]\nst FileName = "%s"\n' % first
    if second:
        out += '[Group1Mission0]\nst FileName = "%s"\n' % second
    return out


def test_kept_stage_copies_independent_bookmarks(tmp_path):
    source, target = tmp_path / "base", tmp_path / "stage"
    bookmark = _w(source / "saved_bookmarks/mc2_10.json", "camera")
    ms.stage_bookmarks(source, target, False)
    assert not target.exists()
    ms.stage_bookmarks(source, target, True)
    copied = target / "saved_bookmarks/mc2_10.json"
    assert copied.read_text() == "camera"
    copied.write_text("edited")
    assert bookmark.read_text() == "camera"
    ms.stage_bookmarks(tmp_path / "missing", target, True)


# --- mission selection -------------------------------------------------------

def test_campaign_first_mission_wins(tmp_path):
    mod = tmp_path / "Exodus"
    _w(mod / "data/campaign/exodus.fit", _campaign("A_0_1", "A_0_2"))
    _w(mod / "data/missions/a_0_1.fit")
    _w(mod / "data/missions/mc2_01.fit")
    _w(mod / "data/missions/mc2_01.pak")
    assert ms.pick_mission([mod]) == ("a_0_1", "campaign:Exodus")


def test_campaign_skips_missing_mission(tmp_path):
    mod = tmp_path / "m"
    _w(mod / "data/campaign/campaign.fit", _campaign("gone", "here"))
    _w(mod / "data/missions/here.fit")
    assert ms.pick_mission([mod])[0] == "here"


def test_no_campaign_uses_first_fit_pak_pair(tmp_path):
    mod = tmp_path / "m"
    _w(mod / "data/missions/mc2_10.fit")
    _w(mod / "data/missions/mc2_10.pak")
    _w(mod / "data/missions/mc2_02.fit")
    _w(mod / "data/missions/mc2_02.pak")
    _w(mod / "data/missions/mc2_01_specials.fit")
    _w(mod / "data/missions/orphan.fit")
    assert ms.pick_mission([mod]) == ("mc2_02", "first-mission")


def test_override_must_exist(tmp_path):
    mod = tmp_path / "m"
    _w(mod / "data/missions/a.fit")
    assert ms.pick_mission([mod], "A") == ("a", "override")
    try:
        ms.pick_mission([mod], "nope")
    except ValueError:
        return
    raise AssertionError("expected ValueError")


def test_addon_campaign_beats_base_mod(tmp_path):
    base, addon = tmp_path / "MCO", tmp_path / "Carver"
    _w(base / "data/campaign/campaign.fit", _campaign("a_0_1"))
    _w(base / "data/missions/a_0_1.fit")
    _w(addon / "data/campaign/carver.fit", _campaign("mc2_01"))
    _w(addon / "data/missions/mc2_01.fit")
    assert ms.pick_mission([base, addon]) == ("mc2_01", "campaign:Carver")


def test_mission_only_in_fst_is_found(tmp_path, monkeypatch):
    mod = tmp_path / "REAP"
    _w(mod / "mission.fst")
    monkeypatch.setattr(ms, "_fst_mission_stems",
                        lambda fst: ({"mc2_05", "mc2_06"}, {"mc2_05", "mc2_06"}))
    assert ms.pick_mission([mod]) == ("mc2_05", "first-mission")


# --- staging -----------------------------------------------------------------

def test_stage_layers_and_isolation(tmp_path):
    cache = tmp_path / "cache"
    _w(cache / "data/shared.txt", "engine")
    _w(cache / "mc2.exe", "exe")
    _w(cache / "system.cfg", "engine cfg")
    mod = tmp_path / "mod"
    _w(mod / "data/shared.txt", "mod")
    _w(mod / "data/own.txt", "own")
    _w(mod / "data/missions/a.fit", "fit")
    _w(mod / "art.fst", "fst")
    _w(mod / "system.cfg", "must not copy")
    stage = tmp_path / "stage"
    ms.stage_mod(cache, [mod], stage, "a")
    assert (stage / "mc2.exe").read_text() == "exe"
    assert (stage / "data/shared.txt").read_text() == "mod"
    assert (cache / "data/shared.txt").read_text() == "engine"   # cache inode untouched
    assert (stage / "data/own.txt").read_text() == "own"
    assert (stage / "art.fst").is_file()
    assert (stage / "system.cfg").read_text() == "engine cfg"


def test_addon_stacks_on_base_mod(tmp_path):
    cache = tmp_path / "cache"
    _w(cache / "mc2.exe", "exe")
    base, addon = tmp_path / "MCO", tmp_path / "Carver"
    _w(base / "data/a.txt", "base")
    _w(base / "data/b.txt", "base")
    _w(base / "tgl.fst", "tgl")
    _w(addon / "data/b.txt", "addon")
    _w(addon / "data/missions/m.fit")
    stage = tmp_path / "stage"
    ms.stage_mod(cache, [base, addon], stage, "m")
    assert (stage / "data/a.txt").read_text() == "base"
    assert (stage / "data/b.txt").read_text() == "addon"
    assert (stage / "tgl.fst").is_file()


def test_missing_data_dir_is_an_error(tmp_path):
    mod = tmp_path / "m"
    mod.mkdir()
    try:
        ms.overlay_mod(mod, tmp_path / "s")
    except FileNotFoundError:
        return
    raise AssertionError("expected FileNotFoundError")


def test_place_file_replaces_hardlink(tmp_path):
    a = _w(tmp_path / "a.txt", "A")
    b = tmp_path / "b.txt"
    assert ms.place_file(a, b) == "link"
    src2 = _w(tmp_path / "c.txt", "C")
    ms.place_file(src2, b)
    assert a.read_text() == "A" and b.read_text() == "C"


def test_engine_payload_comes_from_deploy_manifest():
    # The shaders and exe come from deploy_payload.enumerate_payload, not a local list.
    build = ROOT / "build64-ninja"
    if not (build / "mc2.exe").is_file():
        return
    items = ms.engine_payload_items(build, build)
    rels = {r.lower() for _, r in items}
    assert "mc2.exe" in rels
    assert any(r.startswith("shaders/") for r in rels)
    assert any(r.startswith("data/defs/") for r in rels)
    assert "mc2-launcher.exe" not in rels


def test_select_mods_and_unknown():
    assert [r[0] for r in ms.select_mods("MC2X-REAP3")] == ["MC2X-REAP3"]
    assert len(ms.select_mods("")) == 6
    try:
        ms.select_mods("nope")
    except ValueError:
        return
    raise AssertionError("expected ValueError")


# --- health ------------------------------------------------------------------

GOOD = (
    "[MECHBATCHER v1] event=summary frame=10 eligible=4 submitted=4 buckets=2 draw_calls=2 instances=7 fallback_total=0\n"
    "[MOD_HEALTH v1] event=brain_runs count=1 scenarioTime=0.50\n"
)


def test_health_good():
    h = ms.analyze_health(GOOD)
    assert h["mech_ok"] and h["ai_ok"] and h["mech_instances_max"] == 7


def test_health_vk_instances_count():
    h = ms.analyze_health("[VK_MECH_ISLAND] event=cull_witness seq=1 vk_instances=3 runs=1\n" + GOOD.splitlines()[1])
    assert h["mech_ok"]


def test_health_invisible_mechs():
    h = ms.analyze_health("instances=0\nmlr_mech_draws=0\n[MOD_HEALTH v1] event=brain_runs count=1 scenarioTime=1.0\n")
    assert not h["mech_ok"] and h["ai_ok"]


def test_health_dead_ai():
    h = ms.analyze_health("instances=5\n")
    assert h["mech_ok"] and not h["ai_ok"]


def test_health_late_ai_fails():
    h = ms.analyze_health("instances=5\n[MOD_HEALTH v1] event=first_general_order scenarioTime=99.0\n")
    assert not h["ai_ok"]


def test_result_word():
    ok = {"exit": 0, "health": ms.analyze_health(GOOD)}
    assert ms.result_word(ok) == "PASS"
    assert ms.result_word({"exit": 1, "health": ok["health"]}).startswith("FAIL")
    assert ms.result_word({"exit": 0, "health": ms.analyze_health("")}) == "HEALTH-FAIL"


def test_first_mission_prefers_numbered_over_test_maps(tmp_path):
    mod = tmp_path / "m"
    for stem in ("ai_glenn", "cityassault", "m0101", "reap3m02", "reap3m01"):
        _w(mod / f"data/missions/{stem}.fit")
        _w(mod / f"data/missions/{stem}.pak")
    assert ms.pick_mission([mod])[0] == "reap3m01"
    _w(mod / "data/missions/mc2_03.fit")
    _w(mod / "data/missions/mc2_03.pak")
    assert ms.pick_mission([mod])[0] == "mc2_03"


def _write_fst(path: Path, members: dict) -> None:
    import struct
    names = list(members)
    off = 8 + 266 * len(names)
    table, blobs = b"", b""
    for n in names:
        data = members[n]
        table += struct.pack("<IIII250s", off + len(blobs), len(data), len(data), 0, n.encode())
        blobs += data
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<II", 0xCADDECAF, len(names)) + table + blobs)


def test_stock_gap_fills_from_base_fst_but_never_overrides(tmp_path):
    base = tmp_path / "base"
    _write_fst(base / "camera.fst", {
        "data/cameras/cameras.fit": b"stock cams",
        "data/art/both.fit": b"stock both",
        "data/movies/big.bik": b"movie",
        "data/m/x.beauty/a.png": b"png"})
    stage = tmp_path / "stage"
    _write_fst(stage / "camera.fst", {"data/cameras/mc2xcams.fit": b"mod cams"})  # renamed member
    _w(stage / "data/art/both.fit", "mod")
    stats = ms.fill_stock_gap(base, stage)
    assert (stage / "data/cameras/cameras.fit").read_bytes() == b"stock cams"
    assert (stage / "data/art/both.fit").read_text() == "mod"
    assert not (stage / "data/movies/big.bik").exists()
    assert not (stage / "data/m/x.beauty/a.png").exists()
    assert stats["from_fst"] == 1


def test_layer_refresh_updates_deletes_and_preserves_unchanged(tmp_path):
    source, cache = tmp_path / 'source', tmp_path / 'cache'
    _w(source / 'data/a.fit', 'old')
    _w(source / 'data/remove.fit')
    _w(source / 'data/keep.pak')
    ms.refresh_layer(source, cache)
    inode = (cache / 'data/keep.pak').stat().st_ino
    _w(source / 'data/a.fit', 'changed')
    (source / 'data/remove.fit').unlink()
    ms.refresh_layer(source, cache)
    assert (cache / 'data/a.fit').read_text() == 'changed'
    assert not (cache / 'data/remove.fit').exists()
    assert (cache / 'data/keep.pak').stat().st_ino == inode
    (cache / 'data/a.fit').write_text('private')
    assert (source / 'data/a.fit').read_text() == 'changed'


def test_stock_gap_cache_reuses_extraction_and_invalidates_archive(tmp_path, monkeypatch):
    base, cache = tmp_path / 'base', tmp_path / 'cache'
    archive = base / 'camera.fst'
    _write_fst(archive, {'data/cameras/a.fit': b'first'})
    ms.fill_stock_gap(base, tmp_path / 'one', cache)
    import fst
    original = fst.decompress_entry
    def no_extract(*a):
        raise AssertionError('unchanged FST decompressed twice')
    monkeypatch.setattr(fst, 'decompress_entry', no_extract)
    ms.fill_stock_gap(base, tmp_path / 'two', cache)
    assert (tmp_path / 'two/data/cameras/a.fit').read_bytes() == b'first'
    monkeypatch.setattr(fst, 'decompress_entry', original)
    _write_fst(archive, {'data/cameras/a.fit': b'new payload'})
    ms.fill_stock_gap(base, tmp_path / 'three', cache)
    assert (tmp_path / 'three/data/cameras/a.fit').read_bytes() == b'new payload'
    (tmp_path / 'two/data/cameras/a.fit').write_text('runtime')
    ms.fill_stock_gap(base, tmp_path / 'four', cache)
    assert (tmp_path / 'four/data/cameras/a.fit').read_bytes() == b'new payload'


def test_health_artifacts_are_scoped_to_worker(tmp_path):
    _w(tmp_path / 'one/stamp/mc2_01.log', GOOD)
    _w(tmp_path / 'two/stamp/mc2_01.log', 'instances=0')
    assert ms.analyze_health(ms.artifact_log_text(0, 'mc2_01', tmp_path / 'one'))['ai_ok']
    assert not ms.analyze_health(ms.artifact_log_text(0, 'mc2_01', tmp_path / 'two'))['ai_ok']


def test_run_mod_preserves_head_default_and_forwards_explicit_reference(tmp_path, monkeypatch):
    from types import SimpleNamespace
    seen = []
    def run(cmd, **kw):
        seen.append((cmd, kw['env']))
        return SimpleNamespace(returncode=0, stdout=GOOD, stderr='')
    monkeypatch.setattr(ms.subprocess, 'run', run)
    stage = tmp_path / 'mod'
    stage.mkdir()
    ms.run_mod(stage, 'mc2_01', 5, 60)
    ms.run_mod(stage, 'mc2_01', 5, 60, tmp_path / 'reference.exe')
    assert '--ref-exe' not in seen[0][0]
    assert seen[1][0][-2:] == ['--ref-exe', str(tmp_path / 'reference.exe')]
    assert seen[0][1]['MC2_SMOKE_ARTIFACT_ROOT'] == str(tmp_path / 'artifacts/mod')



"""Unit tests for tools/mod_smoke.py: staging, mission selection, health parsing."""
import importlib.util
import os
import sys
import pytest
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


def test_readonly_stage_cleanup(tmp_path):
    import stat
    staged = _w(tmp_path / 'stage/readonly.exr')
    staged.chmod(stat.S_IREAD)
    ms.shutil.rmtree(staged.parent, onexc=ms.remove_readonly)
    assert not staged.parent.exists()


def test_cleanup_preserves_other_permission_failures(tmp_path):
    staged = _w(tmp_path / 'writable.exr')
    failure = PermissionError('locked by another process')
    with pytest.raises(PermissionError, match='locked by another process'):
        ms.remove_readonly(os.unlink, staged, failure)
    assert staged.exists()


def test_content_budget_is_enforced_and_visible():
    row = dict(mod="fixture", mission="broken", exit=0,
               health=ms.analyze_health(""), content_errors=2, content_budget=1)
    row["health"].update(mech_ok=True, ai_ok=True)
    assert ms.result_word(row) == "CONTENT-FAIL"
    row["content_budget"] = 2
    assert ms.result_word(row) == "PASS"
    assert "2 / 2" in ms.format_table([row])


def test_content_signatures_reject_replacement_error_in_same_class():
    known_line = '[CONTENT] kind=ammo path=unit msg=orphan ammo 207; zeroing amount'
    summary = '[CONTENT_SUMMARY] mission=m total=1 ammo=1'
    counts = ms.content_signatures(known_line + '\n' + known_line + '\n' + summary)
    signature, = counts
    entry = dict(mod='mod', mission='m', error_class='ammo', message_sha256=signature.split(':')[1],
                 count_ceiling=1, first_window=92, expires_window=95)
    assert ms.content_verdict(counts, 'mod', 'm', 92, [entry]) == (counts, {})
    changed = ms.content_signatures(known_line.replace('207', '208') + '\n' + summary)
    assert ms.content_verdict(changed, 'mod', 'm', 92, [entry]) == ({}, changed)
    assert ms.content_signatures(summary) == {'ammo:unparsed': 1}


def test_split_ammo_budget_enforces_each_hash(capsys):
    entries = [e for e in ms.load_known_failures()
               if e['mod'] == 'MC2X-CVE-G' and e['error_class'] == 'ammo']
    assert sorted(e['count_ceiling'] for e in entries) == [1, 1, 5]
    assert sum(e['count_ceiling'] for e in entries) == 7
    for entry in entries:
        signature = 'ammo:' + entry['message_sha256']
        counts = {signature: entry['count_ceiling']}
        assert ms.content_verdict(counts, entry['mod'], entry['mission'], entry['first_window'], entries) == (counts, {})
        counts[signature] += 1
        assert ms.content_verdict(counts, entry['mod'], entry['mission'], entry['first_window'], entries) == ({}, counts)
        assert f"observed={counts[signature]} ceiling={entry['count_ceiling']}" in capsys.readouterr().out
        row = dict(exit=0, content_new=counts)
        assert ms.result_word(row) == 'CONTENT-FAIL(NEW)'


@pytest.mark.parametrize('note', [None, '', '   '])
def test_known_failure_requires_owner_note(tmp_path, note):
    import json
    entry = dict(ms.load_known_failures()[0])
    entry['owner_note'] = note
    ledger = tmp_path / 'renewal.json'
    ledger.write_text(json.dumps([entry]))
    with pytest.raises(ValueError, match='invalid known content failure'):
        ms.load_known_failures(ledger)
    entry['owner_note'] = 'Owner accepts this known error'
    ledger.write_text(json.dumps([entry]))
    assert ms.load_known_failures(ledger) == [entry]


def _campaign(first: str, second: str = "") -> str:
    out = '[Campaign]\nl GroupCount = 2\n[Group0Mission0]\nst FileName = "%s"\n' % first
    if second:
        out += '[Group1Mission0]\nst FileName = "%s"\n' % second
    return out


def test_asset_precheck_json_failure_and_timeout(tmp_path, monkeypatch):
    from types import SimpleNamespace
    import json
    calls = []
    def run(cmd, **kw):
        calls.append((cmd, kw))
        return SimpleNamespace(returncode=1, stdout=json.dumps({'loaded': 2, 'failures': ['FATAL']}))
    monkeypatch.setattr(ms.subprocess, 'run', run)
    assert ms.asset_precheck(tmp_path / 'probe.exe', tmp_path)['failures'] == ['FATAL']
    assert calls[0][1]['timeout'] == 55
    assert calls[0][0][1] == str(tmp_path.resolve())
    for stdout, code in [('not JSON', 0), ('{}', 0), ('{"loaded":0,"failures":[]}', 0),
                         ('{"loaded":2,"failures":[]}', 7)]:
        monkeypatch.setattr(ms.subprocess, 'run', lambda *a, **kw: SimpleNamespace(returncode=code, stdout=stdout, stderr=''))
        assert ms.asset_precheck(tmp_path / 'probe.exe', tmp_path)['failures']
    monkeypatch.setattr(ms.subprocess, 'run', lambda *a, **kw: (_ for _ in ()).throw(ms.subprocess.TimeoutExpired(a[0], 55)))
    assert ms.asset_precheck(tmp_path / 'probe.exe', tmp_path)['failures']


def test_asset_load_real_packet_fixture(tmp_path):
    """Exercise the production ObjectTypeManager; no mission or renderer startup."""
    import json
    import struct
    import pytest
    exe = ROOT / 'build64-ninja/mc2_mod_load.exe'
    if not exe.exists():
        pytest.skip('build mc2_mod_load to exercise the C++ packet fixture')
    _w(tmp_path / 'system.cfg', 'FITini\n[FastFiles]\nl NumFastFiles = 0\nFITend\n')
    pak = tmp_path / 'data/objects/object2.pak'
    packet = (b'FITini\n[ObjectClass]\nl ObjectTypeNum = 1\n'
              b'[BuildingData]\nul DmgLevel = 100\n'
              b'[ObjectType]\nl ExplosionObject = -1\nl DestroyedObject = -1\nFITend\n')
    pak.parent.mkdir(parents=True)
    def write_packet(data):
        # Two packet slots, 0 unused; raw slot 1 begins after the 16-byte header.
        pak.write_bytes(struct.pack('<4I', 0xFEEDFACE, 16, (7 << 29) | 16, 16) + data)
    write_packet(packet)
    report = ms.asset_precheck(exe, tmp_path)
    assert report['loaded'] == 1, report
    assert not report['failures'], report
    write_packet(packet.replace(b'[BuildingData]', b'[MissingData]'))
    report = ms.asset_precheck(exe, tmp_path)
    assert report['failures'], report
    assert 'ObjectTypeManager.load' in report.get('log', ''), report
    pak.unlink()
    assert ms.asset_precheck(exe, tmp_path)['failures']


def test_warning_budget_baselines_and_growth():
    import json
    budgets = json.loads(ms.WARNING_BUDGET.read_text())
    for mod, budget in budgets.items():
        for message, count in budget.items():
            text = '\n'.join('[12:00] STOP ' + message + '!' for _ in range(count))
            counts = ms.warning_counts(text)
            assert ms.warning_excess(counts, mod) == {}
            assert ms.warning_excess(ms.warning_counts(text + '\nSTOP ' + message), mod) == {message: 1}
            assert ms.warning_excess(ms.warning_counts('STOP new error'), mod) == {'new error': 1}


def test_all_requested_diagnostics_fail_even_with_healthy_draw_and_ai():
    for text in ('[FATAL] broken', '[CRASH] access violation', 'ObjectTypeManager.load: failed',
                 'Texture not correct size!', 'STOP bad object'):
        counts = ms.warning_counts(text)
        assert sum(counts.values()) == 1
        row = {'exit': 0, 'health': {'mech_ok': True, 'ai_ok': True},
               'warning_excess': ms.warning_excess(counts, 'MC2-Exodus')}
        assert ms.result_word(row) == 'FAIL(warning budget)'
    assert ms.warning_counts('normal load\nmechs=3') == {}


def test_budget_is_per_mod_and_preserves_new_line_classes():
    assert ms.warning_excess(ms.warning_counts('STOP Texture not correct size!'), 'MCO-Carver5') == {}
    assert ms.warning_excess(ms.warning_counts('STOP Texture not correct size!'), 'MC2-Exodus')
    assert ms.warning_excess(ms.warning_counts('STOP Texture not correct size: NEW'), 'MCO-Carver5')


def test_legacy_stops_without_newlines_count_every_occurrence():
    assert ms.warning_counts('STOPTexture not correct size!' * 3 + '[TerrainLOD v1] ready') == {
        'Texture not correct size': 3}
    assert ms.warning_counts('STOPMech Werewolf has ammo for a weapon it is not carrying!  Ammo Index is 205' * 2) == {
        'Mech Werewolf has ammo for a weapon it is not carrying': 2}
    fatal = ("STOP [FATAL -1091633151]  ObjectTypeManager.load: can't create object  PAUSE "
             'ObjectTypeManager.load: objType NULL post-switch for objTypeNum 1202 — substituting stub BattleMechType ')
    assert ms.warning_counts(fatal * 8 + '[CONTENT_COOK] source=x') == {
        "ObjectTypeManager.load: can't create object": 8}
    assert ms.warning_counts(fatal.replace('—', 'â€\u201d')) == {
        "ObjectTypeManager.load: can't create object": 1}
    assert ms.warning_counts('STOP[PACKET_OOB] packet=2000') == {'[PACKET_OOB] packet=2000': 1}
    assert ms.warning_counts('STOPMech Werewolf has ammo for a weapon it is not carrying!  '
        'Ammo Index is 205logistics: parsed 32 mech files in 4203 ms') == {
            'Mech Werewolf has ammo for a weapon it is not carrying': 1}


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
    monkeypatch.setattr(ms, '_campaign_stems', lambda *a: [])
    assert ms.pick_mission([mod]) == ("mc2_05", "first-mission")


def test_required_campaign_first_and_middle_never_use_stock_fallback(tmp_path):
    import pytest
    for folder, campaign in ms.CAMPAIGNS.items():
        mod = tmp_path / folder
        for stem in ('z_first', 'b_second', 'm_middle', 'a_last', 'mc2_01'):
            _w(mod / f'data/missions/{stem}.fit')
        _w(mod / 'data/campaign/campaign.fit', _campaign('mc2_01'))
        _w(mod / f'data/campaign/{campaign}', _campaign('z_first', 'b_second') +
           _campaign('m_middle', 'a_last'))
        picks = ms.pick_missions([mod], folder)
        assert [stem for stem, _ in picks] == ['z_first', 'm_middle']
        assert picks == ms.pick_missions([mod], folder)
        assert all(campaign in source for _, source in picks)
        assert ms.pick_missions([mod], folder, 'a_last') == [('a_last', 'override')]
        (mod / f'data/campaign/{campaign}').unlink()
        with pytest.raises(ValueError, match='required campaign'):
            ms.pick_missions([mod], folder)


def test_campaign_in_fst_and_loose_campaign_priority(tmp_path):
    import struct
    mod = tmp_path / 'MC2X-REAP3'
    campaign = _campaign('own_first', 'own_middle').encode('latin-1')
    name = b'data\\campaign\\reap3_campaign.fit'
    mod.mkdir()
    (mod / 'mission.fst').write_bytes(struct.pack('<II', 0xCADDECAF, 1) +
        struct.pack('<IIII250s', 274, len(campaign), len(campaign), 0, name) + campaign)
    for stem in ('own_first', 'own_middle', 'loose_first', 'loose_middle'):
        _w(mod / f'data/missions/{stem}.fit')
    assert [s for s, _ in ms.pick_missions([mod], mod.name)] == ['own_first', 'own_middle']
    _w(mod / 'data/campaign/reap3_campaign.fit', _campaign('loose_first', 'loose_middle'))
    assert [s for s, _ in ms.pick_missions([mod], mod.name)] == ['loose_first', 'loose_middle']


def test_gate_mod_duration_is_fifteen_seconds():
    import ast
    tree = ast.parse((ROOT / 'scripts/merge_gate.py').read_text())
    durations = [node.value for node in ast.walk(tree) if isinstance(node, ast.List)
                 and any(isinstance(item, ast.Constant) and item.value == '--stage-root' for item in node.elts)
                 for node in node.elts if isinstance(node, ast.Constant)]
    assert durations[-2:] == ['--duration', '15']


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


def test_staged_runtime_modules_have_private_inodes(tmp_path):
    cache = tmp_path / "cache"
    for name in ("mc2.exe", "avcodec-61.dll", "data/shared.pak"):
        _w(cache / name, "runtime")
    first, second = tmp_path / "first", tmp_path / "second"
    ms.link_engine(cache, first)
    ms.link_engine(cache, second)
    for name in ("mc2.exe", "avcodec-61.dll"):
        assert not os.path.samefile(cache / name, first / name)
        assert not os.path.samefile(first / name, second / name)
    assert os.path.samefile(first / "data/shared.pak", second / "data/shared.pak")


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


def test_cooked_model_cache_links_allow_atomic_runtime_replacement(tmp_path):
    cache = tmp_path / 'cache'
    source = _w(cache / 'cache/content-model/model-key.mci', 'cooked')
    stage = tmp_path / 'stage'
    ms.link_engine(cache, stage)
    target = stage / source.relative_to(cache)
    assert os.path.samefile(source, target)
    replacement = _w(stage / 'cache/content-model/model-key.mci.tmp', 'runtime')
    os.replace(replacement, target)
    assert source.read_text() == 'cooked' and target.read_text() == 'runtime'


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
    assert ms.result_word({"exit": 1, "infra": "timeout", "health": ok["health"]}) == "INFRA(timeout)"


def test_run_mod_timeout_is_infra_and_forwards_grace(tmp_path, monkeypatch):
    """2026-11-08: cold model cooks hit run_smoke's walltime cap before frame 0; that is a host/load result, not a mod FAIL."""
    from types import SimpleNamespace
    seen = []
    capped = "| metropolitan | FAIL | timeout |\nwalltime cap hit at 258.0s\n"
    def run(cmd, **kw):
        seen.append(cmd)
        return SimpleNamespace(returncode=1, stdout=capped, stderr="")
    monkeypatch.setattr(ms.subprocess, "run", run)
    stage = tmp_path / "mod"
    stage.mkdir()
    row = ms.run_mod(stage, "metropolitan", 10, 700, tmp_path / "ref.exe", 0, (), grace_s=480)
    assert row["infra"] == "timeout" and ms.result_word(row) == "INFRA(timeout)"
    assert seen[0][-4:] == ["--grace-s", "480", "--ref-exe", str(tmp_path / "ref.exe")]
    monkeypatch.setattr(ms.subprocess, "run", lambda cmd, **kw: SimpleNamespace(returncode=1, stdout="[CRASH] boom\n", stderr=""))
    assert ms.run_mod(stage, "metropolitan", 10, 60)["infra"] is None


def test_hud_capture_is_opt_in_and_plans_the_skip_and_frame(tmp_path, monkeypatch):
    """--hud-capture is off by default; on, run_mod gets the cinematic skip + one capture frame, never the CLEAN (HUD-off) switch."""
    from types import SimpleNamespace
    seen = []
    monkeypatch.setattr(ms.subprocess, "run", lambda cmd, **kw: seen.append(kw["env"]) or SimpleNamespace(returncode=0, stdout="", stderr=""))
    stage = tmp_path / "mod"
    stage.mkdir()
    ms.run_mod(stage, "mc2_01", 10, 60)
    assert "MC2_VISUAL_CAPTURE_FRAME" not in seen[0] and "MC2_PLAY_MISSION_SKIP_CINEMATICS" not in seen[0]
    out = tmp_path / "hud"
    env = ms.hud_capture_env(out, 600)
    ms.run_mod(stage, "mc2_01", 10, 60, hud_env=env)
    assert out.is_dir()
    assert seen[1]["MC2_VISUAL_CAPTURE_FRAME"] == "600" and seen[1]["MC2_VISUAL_CAPTURE_DIR"] == str(out)
    assert seen[1]["MC2_PLAY_MISSION_SKIP_CINEMATICS"] == "1" and "MC2_VISUAL_CAPTURE_CLEAN" not in seen[1]
    assert ms.collect_hud_png(out, tmp_path / "x.png") is None
    (out / "frame0600.png").write_bytes(b"png")
    assert ms.collect_hud_png(out, tmp_path / "x.png") == str(tmp_path / "x.png") and (tmp_path / "x.png").read_bytes() == b"png"


def test_hud_capture_cli_flags_default_off():
    src = (ROOT / "tools" / "mod_smoke.py").read_text(encoding="utf-8")
    assert "'--hud-capture', action='store_true'" in src and "default=HUD_CAPTURE_FRAME" in src


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
        assert float(cmd[cmd.index('--wait-timeout') + 1]) == 1200
        assert kw['timeout'] == 1260
        return SimpleNamespace(returncode=0, stdout=GOOD, stderr='')
    monkeypatch.setenv('MC2_HOST_LEASE_TIMEOUT', '3600')
    monkeypatch.setattr(ms.subprocess, 'run', run)
    stage = tmp_path / 'mod'
    stage.mkdir()
    ms.run_mod(stage, 'mc2_01', 5, 60)
    ms.run_mod(stage, 'mc2_01', 5, 60, tmp_path / 'reference.exe')
    assert '--ref-exe' not in seen[0][0]
    assert seen[1][0][-2:] == ['--ref-exe', str(tmp_path / 'reference.exe')]
    assert seen[0][1]['MC2_SMOKE_ARTIFACT_ROOT'] == str(tmp_path / 'artifacts/mod')


def test_run_mod_counts_saved_log_once_and_keeps_harness_crashes(tmp_path, monkeypatch):
    from types import SimpleNamespace
    text = GOOD + '\nSTOPTexture not correct size!'
    monkeypatch.setattr(ms, 'artifact_log_text', lambda *a: text)
    monkeypatch.setattr(ms.subprocess, 'run', lambda *a, **kw:
        SimpleNamespace(returncode=0, stdout=text + '\n[CRASH] harness failure', stderr=''))
    result = ms.run_mod(tmp_path, 'fixture', 15, 60)
    assert result['warnings'] == {'Texture not correct size': 1, 'harness failure': 1}


def test_legacy_media_links_and_configs_remain_private(tmp_path):
    layer, cache, stage = (tmp_path / n for n in ('layer', 'cache', 'stage'))
    for name in ('photo.jpg', 'photo.jpeg', 'movie.bik'):
        _w(layer / 'data' / name)
    _w(layer / 'data/types.fit', 'private')
    _w(layer / 'prefs.cfg', 'private')
    ms.refresh_layer(layer, cache)
    stats = ms.overlay_mod(cache, stage, cached_links=True)
    assert stats['linked'] == 3
    for name in ('photo.jpg', 'photo.jpeg', 'movie.bik'):
        assert os.path.samefile(layer / 'data' / name, stage / 'data' / name)
    for name in ('data/types.fit', 'prefs.cfg'):
        (stage / name).write_text('runtime')
        assert (layer / name).read_text() == (cache / name).read_text() == 'private'


def test_cross_volume_layer_cache_materializes_once_then_hardlinks(tmp_path, monkeypatch):
    layer, cache, stage = (tmp_path / n for n in ('layer', 'cache', 'stage'))
    source = _w(layer / 'data/objects.pak', 'packed')
    cached = cache / 'data/objects.pak'
    cached.parent.mkdir(parents=True)
    cached.symlink_to(source)  # migrate the previous cross-volume cache format
    monkeypatch.setattr(ms, 'same_volume', lambda *a: False)
    ms.refresh_layer(layer, cache)
    assert not cached.is_symlink()
    assert not os.path.samefile(source, cached)
    monkeypatch.setattr(ms.shutil, 'copy2', lambda *a: (_ for _ in ()).throw(AssertionError('warm payload copied')))
    ms.refresh_layer(layer, cache)
    ms.overlay_mod(cache, stage, cached_links=True)
    assert os.path.samefile(cached, stage / 'data/objects.pak')




def test_old_cooked_cache_is_discarded_and_not_reused(tmp_path):
    old = _w(tmp_path / 'cooked/model-old-version.mci', 'stale')
    keep = _w(tmp_path / 'layers/mod/source.ase', 'source')
    ms.discard_legacy_cooked_cache(tmp_path)
    assert not old.parent.exists()
    assert keep.read_text() == 'source'
    ms.discard_legacy_cooked_cache(tmp_path)


@pytest.mark.parametrize('asset_mode', [None, 'fail', 'pass'])
@pytest.mark.parametrize('in_gate', [False, True])
def test_single_worker_waits_for_cold_run_and_collects_later_failures(tmp_path, monkeypatch, asset_mode, in_gate):
    from types import SimpleNamespace
    import threading
    source, base, stages = tmp_path / 'source', tmp_path / 'base', tmp_path / 'stages'
    for folder in ('one', 'two'):
        (source / folder).mkdir(parents=True)
    base.mkdir()
    monkeypatch.delenv('MC2_HOST_LEASE_BATCH', raising=False)
    monkeypatch.setenv('MC2_IN_GATE', '1' if in_gate else '0')
    monkeypatch.setattr(ms, 'select_mods', lambda _: [('One', 'one', ''), ('Two', 'two', '')])
    monkeypatch.setattr(ms, 'mod_layers', lambda root, folder, _: [root / folder])
    monkeypatch.setattr(ms, 'pick_mission', lambda *a: ('mc2_01', 'fixture'))
    monkeypatch.setattr(ms, 'pick_missions', lambda *a: [('mc2_01', 'fixture'), ('mc2_02', 'middle')])
    monkeypatch.setattr(ms, 'ensure_loose_mission', lambda *a: False)
    monkeypatch.setattr(ms, 'resolve_build', lambda *a: (base / 'mc2.exe', base))
    monkeypatch.setattr(ms.stages, 'preflight', lambda *a: None)
    monkeypatch.setattr(ms.stages, 'prune_old', lambda *a, **kw: None)
    monkeypatch.setattr(ms, 'build_engine_cache', lambda *a: {})
    monkeypatch.setattr(ms, 'refresh_layer', lambda *a: None)
    monkeypatch.setattr(ms, 'stage_bookmarks', lambda *a: None)
    monkeypatch.setattr(ms, '_load_deploy_payload', lambda: SimpleNamespace(ensure_terrain_materials=lambda *a: None))
    monkeypatch.setattr(ms, 'tree_identity', lambda *a: {})
    monkeypatch.setattr(ms, 'format_table', lambda rows: '')
    monkeypatch.setattr(ms, 'result_word', lambda row: 'PASS' if row['exit'] == 0 else 'FAIL')
    monkeypatch.setattr(ms, 'health_cell', lambda *a: '')
    finished = threading.Event()
    calls = []
    def stage(cache, layers, target, *a, **kw):
        if target.name == 'two':
            assert finished.is_set(), 'second staging overlaps first cold load'
        target.mkdir()
        (target / 'smoke.log').write_text('fixture')
        calls.append('stage-' + target.name)
        return {}
    def run(target, *a):
        assert a[1] == 15
        assert a[2] == (90 if in_gate else 300)
        if target.name == 'one':
            threading.Event().wait(0.1)
            finished.set()
        calls.append('run-' + target.name)
        return {'exit': int(target.name == 'one'), 'health': {}}
    monkeypatch.setattr(ms, 'stage_mod', stage)
    monkeypatch.setattr(ms, 'run_mod', run)
    def precheck(exe, target):
        calls.append('asset-' + target.name)
        finished.set()
        return {'loaded': 1, 'failures': ['FATAL'] if asset_mode == 'fail' else []}
    monkeypatch.setattr(ms, 'asset_precheck', precheck)
    args = SimpleNamespace(install_root=source, stage_root=stages, base_install=base,
                           exe=None, build_dir=None, mod='', mission=None, keep_stages=False,
                           workers=1, fail_fast=False, duration=15, timeout=300, window=90, front_end=False)
    if asset_mode:
        args.asset_load_exe = base / 'probe.exe'
    assert ms.battery(args) == 1
    expected = ['stage-one', 'run-one', 'stage-two', 'run-two']
    if asset_mode == 'fail':
        expected = ['stage-one', 'asset-one', 'stage-two', 'asset-two']
    elif asset_mode == 'pass':
        expected = ['stage-one', 'asset-one', 'run-one', 'stage-two', 'asset-two', 'run-two']
    if not in_gate:
        expected = [item for item in expected for _ in range(2 if item.startswith('run-') else 1)]
    assert calls == expected
    import json
    report = next(stages.glob('*/battery.json'))
    assert len(json.loads(report.read_text())) == (2 if in_gate else 4)


def test_known_rows_apply_on_a_mission_override_unless_mission_only():
    line = "[CONTENT] kind=ammo path=unit msg=orphan ammo 207; zeroing amount"
    counts = ms.content_signatures(line + "\n[CONTENT_SUMMARY] mission=other total=1 ammo=1")
    sig, = counts
    row = dict(mod="mod", mission="default_m", error_class="ammo", message_sha256=sig.split(":")[1],
               count_ceiling=1, first_window=92, expires_window=95)
    assert ms.content_verdict(counts, "mod", "other", 92, [row]) == (counts, {})              # --mission override
    assert ms.content_verdict(counts, "mod", "other", 92, [dict(row, mission_only=True)]) == ({}, counts)
    assert ms.content_verdict(counts, "mod", "default_m", 92, [dict(row, mission_only=True)]) == (counts, {})
    assert ms.content_verdict(counts, "other_mod", "other", 92, [row]) == ({}, counts)         # still keyed to the mod



@pytest.mark.parametrize("winerror", [32, 5])
def test_stage_cleanup_retries_held_files_then_warns_without_failing(tmp_path, monkeypatch, capsys, winerror):
    """W116 item 6: a hardlinked payload still held open after the games end is retried, then left with a warning."""
    staged = tmp_path / "staged"
    staged.mkdir()
    calls = []
    def held(path, onexc=None):
        calls.append(path)
        error = OSError(13, "in use")
        error.winerror = winerror
        raise error
    monkeypatch.setattr(ms.shutil, "rmtree", held)
    monkeypatch.setattr(ms.time, "sleep", lambda s: None)
    assert ms.remove_disposable(staged, attempts=3) is False
    assert len(calls) == 3 and "WARN: could not delete" in capsys.readouterr().out


def test_stage_cleanup_succeeds_after_a_transient_lock(tmp_path, monkeypatch):
    staged = tmp_path / "staged"
    (staged / "sub").mkdir(parents=True)
    (staged / "sub" / "mc2.exe").write_bytes(b"x")
    real, calls = ms.shutil.rmtree, []
    def flaky(path, onexc=None):
        calls.append(path)
        if len(calls) == 1:
            raise PermissionError(13, "WinError 32")
        return real(path, onexc=onexc)
    monkeypatch.setattr(ms.shutil, "rmtree", flaky)
    monkeypatch.setattr(ms.time, "sleep", lambda s: None)
    assert ms.remove_disposable(staged) is True and not staged.exists() and len(calls) == 2


def test_stage_cleanup_keeps_other_errors_loud(tmp_path, monkeypatch):
    def broken(path, onexc=None):
        raise OSError(22, "invalid")
    monkeypatch.setattr(ms.shutil, "rmtree", broken)
    with pytest.raises(OSError):
        ms.remove_disposable(tmp_path)


def _lease_stub(monkeypatch, hold):
    from smoke_lib import host_lease
    calls = []
    monkeypatch.delenv(host_lease.BATCH_ENV, raising=False)
    monkeypatch.setattr(host_lease, "hold", lambda tag, **kw: (calls.append(("hold", tag)), hold(tag))[1])
    monkeypatch.setattr(host_lease, "release_batch", lambda tag: calls.append(("release", tag)))
    return host_lease, calls


def test_battery_takes_one_lease_for_all_missions(monkeypatch):
    host_lease, calls = _lease_stub(monkeypatch, lambda tag: 1234)
    seen = []
    monkeypatch.setattr(ms, "battery", lambda args: seen.append(os.environ.get(host_lease.BATCH_ENV)) or 0)
    assert ms.battery_leased(object()) == 0
    assert [c[0] for c in calls] == ["hold", "release"]
    assert seen == [calls[0][1]]                      # children inherit the batch tag, so they never queue again
    assert host_lease.BATCH_ENV not in os.environ     # restored afterwards


def test_battery_under_existing_batch_takes_no_second_lease(monkeypatch):
    host_lease, calls = _lease_stub(monkeypatch, lambda tag: 1)
    monkeypatch.setenv(host_lease.BATCH_ENV, "outer")
    monkeypatch.setattr(ms, "battery", lambda args: 0)
    assert ms.battery_leased(object()) == 0
    assert calls == []


def test_battery_lease_never_obtained_is_not_run(monkeypatch, capsys):
    def busy(tag):
        raise host_lease.HostLeaseBusy("held by another lane")
    host_lease, calls = _lease_stub(monkeypatch, busy)
    monkeypatch.setattr(ms, "battery", lambda args: pytest.fail("battery must not start"))
    assert ms.battery_leased(object()) == 3
    out = capsys.readouterr().out
    assert "NOT RUN" in out and "FAIL" not in out
    assert [c[0] for c in calls] == ["hold"]

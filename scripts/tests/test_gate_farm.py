"""Farm scheduling and affected Classic save coverage, entirely offline."""
from contextlib import contextmanager
import json
from pathlib import Path
import sys
import threading
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate


def test_finale_keeps_windowed_audit_and_tier1_coverage():
    steps = gate.affected_plan(gate.plan(6), ['mclib/msl_load.cpp'])
    command = next(c for n, c, _ in steps if n == 'determinism-mc2_24')
    assert '--headless-both' in command   # same 2601-frame chain as windowed (34 s against 121 s); the overlap worker still runs
    assert int(command[command.index('--ticks') + 1]) > 2404
    tier1 = next(c for n, c, _ in steps if n == 'tier1')
    full = next(c for n, c, _ in steps if n == 'tier1-full')
    from scripts.smoke_lib.manifest import parse_manifest
    missions = {e.stem for e in parse_manifest(gate.ROOT / 'tests/smoke/smoke_missions.txt') if e.tier == 'tier1'}
    assert 'mc2_24' in missions
    # p29 cut 4: every window runs day + night; the five-map breadth (mc2_24 included) is the weekly tier1-full row
    assert [tier1[i + 1] for i, arg in enumerate(tier1) if arg == '--mission'] == ['mc2_01', 'mc2_10']
    assert gate.stage_tier('tier1') == 0 and gate.stage_tier('tier1-full') == 2
    if '--mission' in full:
        assert 'mc2_24' in [full[i + 1] for i, arg in enumerate(full) if arg == '--mission']


def test_weekly_only_stages_are_tier2():
    for n in ('tier1-full', 'quiet', 'saveload-mc2_03', 'saveload-mc2_16', 'saveload-mc2_24'):
        assert gate.stage_tier(n) == 2, n
    for n in ('saveload-mc2_01', 'saveload-mc2_10', 'determinism-mc2_10', 'player-path', 'tier1'):
        assert gate.stage_tier(n) < 2, n


@pytest.mark.parametrize('path,classic', [
    ('code/mission_update.cpp', False), ('RenderCore/vk_sync.cpp', False),
    ('assets/rock.png', False), ('unknown/file.xyz', False),
    ('code/logistics_save.cpp', False), ('code/mission_load.cpp', False),
    ('tests/saveload/baseline.json', False), ('mclib/bdactor.cpp', False),
    ('mclib/render_options.cpp', False), ('mods/classic/assets.json', False),
    ('shaders/classic.frag', False),
])
def test_classic_save_repeats_only_for_affected_domains(path, classic):
    steps = gate.affected_plan(gate.plan(6), [path])
    saves = [(n, e) for n, _, e in steps if n.startswith('saveload-')]
    assert any(n.endswith('-classic') for n, _ in saves) == classic
    assert any(not e.get('MC2_GATE_SKIP_REASON') for n, e in saves if not n.endswith('-classic')) == (path != 'shaders/classic.frag')
    mods = next(c for n, c, _ in steps if n == 'mods')
    assert mods[mods.index('--workers') + 1] == '2'


def test_classic_docs_do_not_schedule_runtime_proofs():
    steps = gate.affected_plan(gate.plan(6), ['docs/classic-save.md'])
    assert not any(n.endswith('-classic') for n, _, _ in steps)


def test_mod_payload_cache_survives_random_candidate_names(tmp_path, monkeypatch):
    monkeypatch.setattr(gate, 'ROOT', tmp_path / 'lane-c')
    roots = []
    for candidate in ('stock-random-one', 'stock-random-two'):
        steps = gate.install_plan(gate.plan(6), tmp_path / candidate)
        command = next(c for n, c, _ in steps if n == 'mods')
        roots.append(command[command.index('--stage-root') + 1])
    assert roots[0] == roots[1]
    assert roots[0].endswith('lane-c-mods-stage')
    monkeypatch.setattr(gate, 'ROOT', tmp_path / 'other-lane')
    command = next(c for n, c, _ in gate.install_plan(gate.plan(6), tmp_path / 'candidate') if n == 'mods')
    assert command[command.index('--stage-root') + 1] != roots[0]


@pytest.mark.parametrize('failure', [None, 'submit', 'wait', 'fetch', 'bad-id'])
def test_farm_stage_preserves_checks_and_fetches_failures(tmp_path, failure):
    out = tmp_path / 'determinism-mc2_10'
    command = [sys.executable, 'tools/sim_determinism.py', 'check', 'mc2_10',
               '--install', 'local', '--out', str(out), '--sim-speed', '2']
    seen = []
    def child(cmd, **kw):
        if cmd == command:
            seen.append('local')
            assert kw['env']['MC2_VULKAN_VALIDATION'] == '1'
            return SimpleNamespace(returncode=0, stdout='local proof')
        action = cmd[2]
        seen.append(action)
        if action == 'submit':
            job = json.loads(Path(cmd[3]).read_text())
            remote = job['argv']
            assert remote[2:4] == ['check', 'mc2_10']
            assert remote[-4:] == ['--sim-speed', '2', '--ref-exe', '{EXE}']
            assert remote[remote.index('--out') + 1].startswith('{RESULT}')
            assert job['env']['MC2_VULKAN_VALIDATION'] == '1'
            assert job['env']['MC2_HOST_LEASE_PATH'].startswith('C:/mc2farm/')
            assert 'MC2_GATE_BASE' not in job['env']
            assert cmd[-1] == str(gate.ROOT / 'build64-ninja/mc2.exe')
        return SimpleNamespace(returncode=int(action == failure),
                               stdout=('bad id' if failure == 'bad-id' else 'a' * 32) if action == 'submit' else '')
    result = gate.farm_stage(command, {'MC2_VULKAN_VALIDATION': '1', 'MC2_GATE_BASE': 'sha'}, runner=child)
    assert result.returncode == int(failure == 'wait')
    expected = ['alive', 'submit'] if failure in ('submit', 'bad-id') else ['alive', 'submit', 'wait', 'fetch']
    if failure in ('submit', 'bad-id', 'fetch'):
        expected.append('local')
    assert seen == expected


@pytest.mark.parametrize('failure', [None, 'farm', 'save'])
def test_farm_trio_overlaps_serial_saves_after_deploy(tmp_path, monkeypatch, failure):
    saving, farming, mods_done = threading.Event(), threading.Event(), threading.Event()
    calls = []
    @contextmanager
    def copy(install):
        yield tmp_path / 'clone'
    def local(command, **kw):
        name = command[0]
        calls.append(name)
        if name == 'mods':
            mods_done.set()
        elif name == 'save':
            saving.set()
            assert farming.wait(2), 'farm must overlap saves'
        return SimpleNamespace(returncode=int(failure == name))
    def farm(command, overrides, local_env=None):
        assert 'deploy' in calls
        calls.append(command[0])
        farming.set()
        assert saving.wait(2), 'saves must overlap farm'
        return SimpleNamespace(returncode=int(failure == 'farm'), stdout='')
    monkeypatch.delenv('MC2_HOST_LEASE_BATCH', raising=False)
    monkeypatch.setattr(gate, 'git', lambda *a: 'head')
    monkeypatch.setattr(gate, 'log_lease_holders', lambda: None)
    monkeypatch.setattr(gate, 'stock_copy', copy)
    monkeypatch.setattr(gate, 'run_child', local)
    monkeypatch.setattr(gate, 'farm_stage', farm)
    steps = [('deploy', ['deploy'], {}), ('mods', ['mods'], {})]
    steps += [('determinism-' + m, ['farm-' + m, '--install', str(tmp_path / 'install')], {}) for m in ('mc2_03', 'mc2_10', 'mc2_24')]
    steps += [('saveload-mc2_01', ['save', 'tools/saveload_roundtrip.py', '--install', str(tmp_path / 'install')], {})]
    assert gate.execute_parallel(steps, 'head', farm_determinism=True) == int(failure is not None)
    assert len([c for c in calls if c.startswith('farm-')]) == 3
    # Windowed validation must remain on the host even when the farm is selected.
    assert 'mods' in calls and 'save' in calls  # no mods->saveload barrier (private MC2_SAVE_DIR)


def test_farm_queue_timeout_repeats_only_after_pending_job_cancelled(tmp_path):
    seen = []
    def run(command, **kw):
        seen.append(command)
        if command[2] == 'alive':
            return SimpleNamespace(returncode=0, stdout='heartbeat_age=1')
        if len(seen) == 2:
            return SimpleNamespace(returncode=0, stdout='a' * 32)
        if len(seen) == 3:
            assert command[-2:] == ['--queue-timeout', str(gate.FARM_CLAIM_SECONDS)]
            return SimpleNamespace(returncode=1, stdout='queue_timeout: unclaimed job cancelled')
        return SimpleNamespace(returncode=0, stdout='local PASS')
    command = ['python', 'tools/sim_determinism.py', 'check', 'mc2_03', '--install', 'local', '--out', str(tmp_path / 'out')]
    assert gate.farm_stage(command, {}, runner=run).returncode == 0
    assert seen[-1] == command


def test_farm_is_opt_in_and_claim_probe_is_short():
    assert not hasattr(gate, 'choose_farm')
    assert gate.FARM_CLAIM_SECONDS == 30


def test_dead_farm_worker_skips_claim_wait_and_runs_locally(tmp_path):
    seen = []
    def run(command, **kw):
        seen.append(command[2] if len(command) > 2 and 'host.py' in command[1] else 'local')
        return SimpleNamespace(returncode=1 if seen[-1] == 'alive' else 0, stdout='heartbeat_age=-1')
    command = [sys.executable, 'tools/sim_determinism.py', 'check', 'mc2_03', '--install', 'local', '--out', str(tmp_path / 'out')]
    assert gate.farm_stage(command, {}, runner=run).returncode == 0
    assert seen == ['alive', 'local']   # no submit, no wait: the 30 s claim window is never entered


def test_farm_local_fallback_runs_with_the_gate_stage_env(tmp_path, monkeypatch):
    """W115: the fallback dropped execute()'s env, so the gate's own determinism child lost MC2_IN_GATE and GATE QUIET refused it."""
    envs = []
    def run(command, **kw):
        if 'host.py' in command[1]:
            return SimpleNamespace(returncode=1, stdout='heartbeat_age=-1')   # dead worker: straight to local
        envs.append(kw['env'])
        return SimpleNamespace(returncode=0, stdout='')
    command = [sys.executable, 'tools/sim_determinism.py', 'check', 'mc2_24', '--install', 'local', '--out', str(tmp_path / 'out')]
    monkeypatch.setattr(gate, 'git', lambda *a: 'HEAD')
    monkeypatch.setattr(gate, 'ACTIVE_STATE', None)
    monkeypatch.delenv('MC2_IN_GATE', raising=False)
    gate.FAILED_STAGES.clear()
    remote = {gate.farm_key(command): {'MC2_X': '1'}}
    def farm_runner(c, cwd, env):
        return gate.farm_stage(c, remote[gate.farm_key(c)], runner=run, local_env=env)
    assert gate.execute([('determinism-mc2_24', command, {'MC2_X': '1'})], 'HEAD', runner=farm_runner) == 0
    assert envs and envs[0].get('MC2_IN_GATE') == '1' and envs[0].get('MC2_X') == '1'
    assert 'farm_stage(command, remote_envs[farm_key(command)], local_env=env)' in Path(gate.__file__).read_text(encoding='utf-8')


def test_interrupt_is_held_until_the_running_stage_ends(monkeypatch):
    gate.STOP_REQUESTED.clear()
    ran = []
    def runner(command, **kw):
        ran.append(command[0])
        gate.STOP_REQUESTED.set()   # a signal lands mid-stage: that stage still completes
        return SimpleNamespace(returncode=0, stdout='')
    monkeypatch.setattr(gate, 'git', lambda *a: 'HEAD')
    monkeypatch.setattr(gate, 'ACTIVE_STATE', None)
    try:
        rc = gate.execute([('one', ['a'], {}), ('two', ['b'], {})], 'HEAD', runner=runner)
    finally:
        gate.STOP_REQUESTED.clear()
    assert ran == ['a'] and rc


HDR = 'bookmark | FLIP mean | FLIP p99 | MAD | %>24\n'


def test_mad_flags_only_undeclared_views_over_limit():
    out = HDR + 'mc2_01_saved_205644 | 0.0100 | 0.2000 | 1.4000 | 5\nmc2_10_saved_204523 | 0.0900 | 0.5000 | 0.0200 | 0\n'
    assert gate.mad_flags(out) == ['mc2_01_saved_205644=1.4000']
    assert gate.mad_flags(out, {'mc2_01_saved_205644': 2.0}) == []
    assert gate.mad_flags(out, {'mc2_01_saved_205644': 2.0}, expected=('mc2_01_saved_205644', 'mc2_10_saved_204523')) == []


def test_mad_flags_reads_the_mad_column_by_header_not_flip_mean():
    # FLIP mean 0.30 is over MAD_LIMIT but MAD is 0.01: no flag; the reverse must flag.
    assert gate.mad_flags(HDR + 'a | 0.3000 | 0.9000 | 0.0100 | 0\n', expected=('a',)) == []
    assert gate.mad_flags(HDR + 'a | 0.0010 | 0.0100 | 0.9000 | 3\n', expected=('a',)) == ['a=0.9000']
    assert gate.mad_flags(HDR + 'a | n/a | n/a | 0.0100 | 0\n', expected=('a',)) == []  # no FLIP is fine


def test_mad_flags_nan_and_non_numeric_rows_are_flagged():
    assert gate.mad_flags(HDR + 'a | 0.1 | 0.1 | nan | 0\n', expected=('a',))
    assert any('a=' in f for f in gate.mad_flags(HDR + 'a | 0 | 0 | n/a | 0\nb | 0 | 0 | 0.00 | 0\n', expected=('a', 'b')))
    assert gate.mad_flags(HDR + 'b | 0 | 0 | 0.00 | 0\n', expected=('a', 'b')) == ['a=no compare row']


def test_mad_flags_ignores_pixel_hist_lines():
    out = (HDR + 'a | 0.10 | 0.20 | 0.03 | 0\n'
           '[pixel_hist] a: FAIL >8:2.248% region bbox=[0, 95, 601, 183] 0.1127% | 2.2477% of pixels over delta 8 (limit 1.0000%)\n')
    assert gate.mad_flags(out, expected=('a',)) == []


def test_mad_flags_fail_closed_on_missing_capture():
    ok = HDR + 'a | 0 | 0 | 0.00 | 0\nb | 0 | 0 | 0.00 | 0\n'
    assert gate.mad_flags(ok, expected=('a', 'b')) == []
    assert gate.mad_flags(HDR + 'a | 0 | 0 | 0.00 | 0\n', expected=('a', 'b')) == ['b=no compare row']
    assert gate.mad_flags('[ab] b: missing png (a=x b=None)\n' + HDR + 'a | 0 | 0 | 0.00 | 0\n', expected=('a',))
    assert gate.mad_flags('[ab] WARNING a: image size mismatch A=(1, 1) B=(2, 2)\n' + HDR + 'a | 0 | 0 | 0.00 | 0\n')


def test_publish_refuses_without_green_mark(tmp_path, monkeypatch):
    monkeypatch.setattr(gate, 'ROOT', tmp_path)
    assert gate.publish('abc123') == 1


def test_stock_takes_all_saves_after_tier1_and_mods(tmp_path, monkeypatch):
    mods_done, tier_done = threading.Event(), threading.Event()
    saving = threading.Event()
    seen = []
    @contextmanager
    def copy(install):
        yield tmp_path / 'clone'
    def child(command, **kw):
        if command[0] == 'mods':
            mods_done.set()
        elif command[0] == 'tier1':
            tier_done.set()
        elif command[0] == 'determinism':
            assert saving.wait(2), 'saves must overlap the determinism worker'
        elif 'tools/saveload_roundtrip.py' in command:
            install = command[command.index('--install') + 1]
            if install.endswith('install'):
                assert tier_done.is_set()
            seen.append(install)
            saving.set()
        return SimpleNamespace(returncode=0)
    monkeypatch.delenv('MC2_HOST_LEASE_BATCH', raising=False)
    monkeypatch.setattr(gate, 'git', lambda *a: 'head')
    monkeypatch.setattr(gate, 'log_lease_holders', lambda: None)
    monkeypatch.setattr(gate, 'stock_copy', copy)
    monkeypatch.setattr(gate, 'run_child', child)
    steps = [('deploy', ['deploy'], {}), ('tier1', ['tier1'], {}), ('mods', ['mods'], {})]
    steps += [('determinism-mc2_10', ['determinism', '--install', str(tmp_path / 'install')], {})]
    steps += [('saveload-' + str(i), ['python', 'tools/saveload_roundtrip.py', '--install', str(tmp_path / 'install')], {}) for i in range(4)]
    assert gate.execute_parallel(steps, 'head') == 0
    assert sum(p.endswith('clone') for p in seen) == 0
    assert sum(p.endswith('install') for p in seen) == 4


def test_mad_flags_compares_the_raw_value_not_the_rounded_print(tmp_path):
    (tmp_path / 'summary.json').write_text('{"rows": [{"bookmark": "a", "mad": 0.0539}]}')
    out = 'bookmark | MAD | %>24\na | 0.05 | 0\n[ab] wrote ' + str(tmp_path) + '\n'
    assert gate.mad_flags(out) == ['a=0.05']                  # 0.0539 > 0.05 although the table prints 0.05
    assert gate.mad_flags('a | 0.05 | 0\n') == []             # no summary.json: the printed value still decides
    assert gate.mad_flags('a | 0.0539 | 0\n') == ['a=0.0539']  # ab.py now prints 4 decimals


def test_mad_flags_raw_below_printed_never_lowers_a_red(tmp_path):
    (tmp_path / 'summary.json').write_text('{"rows": [{"bookmark": "a", "mad": 0.01}]}')
    out = 'a | 0.30 | 0\n[ab] wrote ' + str(tmp_path) + '\n'
    assert gate.mad_flags(out) == ['a=0.30']


def test_mad_flags_declared_view_needs_a_numeric_row_under_its_bound():
    assert gate.mad_flags('a | nan | 0\n', {'a': 0.5}, expected=('a',))   # declared-nan
    assert gate.mad_flags('b | 0.00 | 0\n', {'a': 0.5}, expected=('a', 'b')) == ['a=no compare row']   # declared-missing-row
    assert gate.mad_flags('a | 0.60 | 0\n', {'a': 0.5}) == ['a=0.60 (declared bound 0.5)']   # declared-over-bound
    assert gate.mad_flags('a | 0.40 | 0\n', {'a'}) == []   # a bare set gets the default 0.5
    assert gate.parse_declared_views(['a', 'b=1.5']) == {'a': 0.5, 'b': 1.5}


def test_declared_view_bad_bound_is_an_argparse_error(capsys, monkeypatch):
    monkeypatch.delenv('MC2_IN_GATE', raising=False)   # the gate's contracts stage sets it
    with pytest.raises(SystemExit) as exc:
        gate.main(['--declared-view', 'a=high'])
    assert exc.value.code == 2
    assert 'not a number' in capsys.readouterr().err


def test_publish_refuses_when_head_is_not_expected(monkeypatch, capsys):
    calls = []
    monkeypatch.setattr(gate, 'git', lambda *a: 'bbbb' if a[-1] == 'HEAD' else 'aaaa')
    monkeypatch.setattr(gate, 'publish', lambda *a: calls.append(a) or 0)
    assert gate.main(['--publish', '--expect-head', 'aaaa']) == 1
    assert not calls and 'refused' in capsys.readouterr().out
    monkeypatch.setattr(gate, 'git', lambda *a: 'aaaa')
    assert gate.main(['--publish', '--expect-head', 'aaaa']) == 0 and calls


def test_farm_local_fallback_waits_for_the_game_row_on_the_same_install(tmp_path):
    """W116 item 5: a farm fallback on the live install never launches beside the mods battery / a stock row there
    (run_smoke's same-exe guard refused it); a row on a clone install does not wait."""
    install, clone = tmp_path / 'install', tmp_path / 'clone'
    order = []
    def run(command, **kw):
        if 'host.py' in command[1]:
            return SimpleNamespace(returncode=1, stdout='heartbeat_age=-1')
        order.append('fallback')
        return SimpleNamespace(returncode=0, stdout='')
    command = [sys.executable, 'tools/sim_determinism.py', 'check', 'mc2_03', '--install', str(install), '--out', str(tmp_path / 'out')]
    held = gate.install_lock(['mod_smoke', '--exe', str(install / 'mc2.exe')])
    assert held is gate.install_lock(command) and held is not gate.install_lock(['x', '--install', str(clone)])
    with held:
        worker = threading.Thread(target=lambda: gate.farm_stage(command, {}, runner=run))
        worker.start()
        worker.join(0.3)
        order.append('battery done')
    worker.join(5)
    assert order == ['battery done', 'fallback']
    with gate.install_lock(['x', '--install', str(clone)]):
        assert gate.farm_stage(command, {}, runner=run).returncode == 0   # another install: no wait


def test_saveload_headless_rows_are_the_proven_ones():
    steps = dict((n, c) for n, c, _ in gate.plan(6))
    assert {m for m in ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24") if "--headless" in steps["saveload-" + m]} == {"mc2_16", "mc2_24"}


def _tier0(planned):
    return [s for s in planned if gate.stage_tier(s[0]) == 0]


@pytest.mark.parametrize('paths,want,single', [
    (['code/mission_update.cpp'], {'determinism-mc2_10', 'determinism-mc2_24', 'saveload-mc2_01', 'saveload-mc2_10'}, False),
    (['code/logisticsdata.cpp'], {'player-path'}, True),
    # W137: logistics saves are sim/save-sensitive (forward plan P1) and still a front-end path.
    (['code/logisticsdata_save.cpp'], {'determinism-mc2_10', 'determinism-mc2_24', 'saveload-mc2_01', 'saveload-mc2_10', 'player-path'}, False),
    (['code/mc2movie.cpp'], {'player-path'}, True),
    (['data/missions/mc2_16.fit'], {'determinism-mc2_16', 'saveload-mc2_16', 'mods'}, True),   # data/ is mod content
    (['RenderCore/vk_sync.cpp'], set(), True),
])
def test_diff_restores_tier_filtered_rows(paths, want, single):
    """p31: tier filtering runs before diff classification; sim, map and front-end changes get their rows back at the merge."""
    scheduled = gate.plan(6)
    steps, promoted = gate.promote_for_diff(_tier0(scheduled), scheduled, _tier0(scheduled), paths, [], False)
    assert promoted == want
    names = [n for n, _, _ in steps]
    assert set(names) == {n for n, _, _ in _tier0(scheduled)} | want
    assert names == [n for n, _, _ in scheduled if n in set(names)]   # plan order kept
    finale = [c for n, c, _ in steps if n == 'determinism-mc2_24']
    assert all(c[c.index('--ticks') + 1] == '2600' for c in finale)
    for n, c, _ in steps:
        if n == 'determinism-mc2_16':
            assert ('--single' in c) == single


def test_operator_stage_choice_is_never_widened():
    scheduled = gate.plan(6)
    steps, promoted = gate.promote_for_diff(_tier0(scheduled)[:1], scheduled, scheduled, ['code/mission_update.cpp'], ['contracts'], False)
    assert promoted == set() and len(steps) == 1


@pytest.mark.parametrize("path", ["mclib/move.cpp", "mclib/movegrid.h", "code/movemgr.cpp", "code/warrior.h"])
def test_movement_is_sim_sensitive(path):
    assert gate.sim_sensitive([path]) == [path]


@pytest.mark.parametrize("path", ["data/missions/mc2_01.fit", "mods/core_unlocks/data/foo.fit",
                                  "examples/mods/sample_variant/data/x.csv", "tests/mods/fixture.fit",
                                  "tools/mod_smoke.py", "tools/mc2x_import/transfer_mc2x_campaign.py",
                                  "tools/mco_import/build_mco_compat.py", "tools/mc2mod/__init__.py",
                                  "tools/mc2_modlint/rules.py", "tools/validate_mod_package.py",
                                  "tools/mod_install/install.py", "tools/mod_load.cpp"])
def test_mod_content_diff_promotes_existing_mod_battery(path):
    scheduled = gate.plan(6)
    steps, promoted = gate.promote_for_diff(_tier0(scheduled), scheduled, _tier0(scheduled), [path], [], False)
    assert "mods" in promoted
    command = next(c for n, c, _ in steps if n == "mods")
    assert "tools/mod_smoke.py" in command
    assert gate.stage_tier("mods") == 2


@pytest.mark.parametrize("path", ["mclib/vfs.cpp", "mclib/ablvm.cpp", "mclib/file.cpp", "mclib/texture_assets.cpp",
                                  "GameOS/gameos/texture_decode.cpp", "RenderCore/TextureService.h",
                                  "RenderCore/vk_sync.cpp", "shaders/vulkan/terrain.frag"])
def test_engine_vfs_loader_diff_never_promotes_mods(path):
    # Owner rule 2026-11-06: engine C++, shaders, the VFS and loaders never trigger mods; the nightly gate covers them.
    assert "mods" not in gate.promoted_stages([path])
    assert gate.stage_tier("mods") == 2


@pytest.mark.parametrize("base", [None, "invalid-base"])
def test_gate_refuses_missing_or_invalid_comparison_base(tmp_path, monkeypatch, capsys, base):
    import subprocess
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    def git(*args):
        if args[0] == "merge-base":
            raise subprocess.CalledProcessError(1, args)
        return "a" * 40
    monkeypatch.setattr(gate, "git", git)
    argv = ["--full", "--dry-run", "--proofs-dir", str(tmp_path / "proof")]
    if base:
        argv += ["--base", base]
    with pytest.raises(SystemExit) as exc:
        gate.main(argv)
    assert exc.value.code == 2
    assert "comparison base" in capsys.readouterr().err


@pytest.mark.parametrize('weekly,paths,single', [(False, [], True), (True, [], False), (False, ['mclib/sim_overlap.h'], False)])
def test_nightly_03_16_single_run_and_pairs_on_weekly_or_sim(weekly, paths, single):
    scheduled = gate.plan(6)
    steps, _ = gate.promote_for_diff(scheduled, scheduled, scheduled, paths, [], weekly)
    for n, c, _ in steps:
        if n in ('determinism-mc2_03', 'determinism-mc2_16'):
            assert ('--single' in c) == single, n
        elif n.startswith('determinism-'):
            assert '--single' not in c, n

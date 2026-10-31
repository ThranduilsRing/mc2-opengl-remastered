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
    assert '--headless-both' not in command
    assert int(command[command.index('--ticks') + 1]) > 2404
    assert command[command.index('--sim-speed') + 1] == '4'
    tier1 = next(c for n, c, _ in steps if n == 'tier1')
    from scripts.smoke_lib.manifest import parse_manifest
    missions = {e.stem for e in parse_manifest(gate.ROOT / 'tests/smoke/smoke_missions.txt') if e.tier == 'tier1'}
    assert 'mc2_24' in missions
    if '--mission' in tier1:
        assert 'mc2_24' in [tier1[i + 1] for i, arg in enumerate(tier1) if arg == '--mission']


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


def test_kit_visibility_is_nightly_but_catching_stages_remain():
    for full in (False, True):
        steps = gate.affected_plan(gate.plan(6), ['code/mission_update.cpp'], full=full)
        env = next(e for n, _, e in steps if n == 'kit-visibility')
        assert bool(env.get('MC2_GATE_SKIP_REASON')) == (not full)
        for name in ('tier1', 'mods', 'determinism-mc2_03', 'determinism-mc2_10', 'determinism-mc2_24', 'saveload-mc2_01', 'saveload-mc2_10'):
            assert not next(e for n, _, e in steps if n == name).get('MC2_GATE_SKIP_REASON')
        assert next(e for n, _, e in steps if n == 'tier1')['MC2_VULKAN_VALIDATION'] == '1'


@pytest.mark.parametrize('failure', [None, 'submit', 'wait', 'fetch', 'bad-id'])
def test_farm_stage_preserves_checks_and_fetches_failures(tmp_path, failure):
    out = tmp_path / 'determinism-mc2_10'
    command = [sys.executable, 'tools/sim_determinism.py', 'check', 'mc2_10',
               '--install', 'local', '--out', str(out), '--sim-speed', '2']
    seen = []
    def child(cmd, **kw):
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
    if failure == 'bad-id':
        with pytest.raises(RuntimeError, match='unique job ID'):
            gate.farm_stage(command, {'MC2_VULKAN_VALIDATION': '1'}, runner=child)
    else:
        result = gate.farm_stage(command, {'MC2_VULKAN_VALIDATION': '1', 'MC2_GATE_BASE': 'sha'}, runner=child)
        assert result.returncode == int(failure is not None)
    assert seen == (['submit'] if failure in ('submit', 'bad-id') else ['submit', 'wait', 'fetch'])


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
            assert mods_done.is_set()
            saving.set()
            assert farming.wait(2), 'farm must overlap saves'
        return SimpleNamespace(returncode=int(failure == name))
    def farm(command, overrides):
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
    assert calls.index('mods') < calls.index('save')


def test_farm_queue_timeout_repeats_only_after_pending_job_cancelled(tmp_path):
    seen = []
    def run(command, **kw):
        seen.append(command)
        if len(seen) == 1:
            return SimpleNamespace(returncode=0, stdout='a' * 32)
        if len(seen) == 2:
            assert command[-2:] == ['--queue-timeout', '1200']
            return SimpleNamespace(returncode=1, stdout='queue_timeout: unclaimed job cancelled')
        return SimpleNamespace(returncode=0, stdout='local PASS')
    command = ['python', 'tools/sim_determinism.py', 'check', 'mc2_03', '--install', 'local', '--out', str(tmp_path / 'out')]
    assert gate.farm_stage(command, {}, runner=run).returncode == 0
    assert seen[-1] == command


@pytest.mark.parametrize('depth,occupied,expected', [(0, 2, True), (1, 2, False), (2, 2, False), (0, 0, True)])
@pytest.mark.parametrize('prefix', ['', '** WARNING: connection is not using a post-quantum key exchange algorithm.\n'])
def test_farm_selection_compares_queue_to_host_wait(tmp_path, monkeypatch, depth, occupied, expected, prefix):
    from smoke_lib import host_lease
    monkeypatch.setattr(host_lease, 'status', lambda: {'slots': [{}] * occupied})
    monkeypatch.setattr(host_lease, 'slot_count', lambda: 2)
    monkeypatch.setattr(host_lease, 'lease_path', lambda: tmp_path / 'lease.json')
    assert gate.choose_farm(lambda *a, **kw: SimpleNamespace(returncode=0, stdout=prefix + json.dumps({'depth': depth}))) == expected


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
            assert mods_done.is_set()
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

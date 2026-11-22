"""Offline input failures must precede expensive merge-window stages."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate
import exe_fingerprint
import mod_smoke as mods


@pytest.mark.parametrize('window,counts,new', [
    (92, {'ammo': 7}, {}), (92, {'shape': 1}, {'shape': 1}),
    (92, {'ammo': 8}, {'ammo': 8}), (200, {'ammo': 7}, {}),
])
def test_known_failure_boundaries(window, counts, new):
    entries = [dict(mod='MC2X-CVE-G', mission='metropolitan', error_class='ammo',
                    message_sha256='a' * 64, count_ceiling=7, first_window=92, expires_window=95)]
    counts = {kind + ':' + 'a' * 64: n for kind, n in counts.items()}
    new = {kind + ':' + 'a' * 64: n for kind, n in new.items()}
    known, actual = mods.content_verdict(counts, 'MC2X-CVE-G', 'metropolitan', window, entries)
    assert actual == new
    assert bool(known) == (not new)


def test_echoed_content_summary_is_not_counted_twice():
    line = '[CONTENT_SUMMARY] mission=m total=2 ammo=1 movetype=1'
    assert mods.content_counts(line + '\n' + line) == {'ammo': 1, 'movetype': 1}


def test_committed_save_references_use_current_policy():
    import saveload_roundtrip
    policy = saveload_roundtrip.mask_policy(str(gate.ROOT / 'tools/save_noise_mask.json'))
    for path in (gate.ROOT / 'tests/saveload/baselines').glob('mc2_*.json'):
        assert json.dumps(json.loads(path.read_text())['comparison'], sort_keys=True) == json.dumps(policy, sort_keys=True)


@pytest.mark.parametrize('broken', [None, 'game', 'viewer', 'camera', 'duplicate', 'equivalent', 'policy', 'seed_identity', 'candidate_identity'])
@pytest.mark.parametrize('scheduled_deploy', [False, True])
def test_preflight_inputs(tmp_path, monkeypatch, capsys, broken, scheduled_deploy):
    monkeypatch.setattr(gate, 'ROOT', tmp_path)
    def fingerprint(path):
        assert path in (tmp_path / 'build64-ninja/mc2.exe',
                        tmp_path / 'build64-ninja/out/tools/asset_viewer/mc2_asset_viewer.exe')
        return dict(present=True, sha='b' * 40 if path.name == {'game': 'mc2.exe', 'viewer': 'mc2_asset_viewer.exe'}.get(broken) else 'a' * 40, dirty=0)
    monkeypatch.setattr(exe_fingerprint, 'extract', fingerprint)
    import deploy_payload
    checked = []
    def identity(target, root, head):
        checked.append(target.name)
        assert root == tmp_path and head == 'a' * 40
        if broken == target.name + '_identity':
            raise ValueError('stale published exe/shaders')
    monkeypatch.setattr(deploy_payload, 'verify_published_identity', identity)
    def published_bytes(target, **_kw):
        checked.append(Path(target).name)
        return dict(has_manifest=True, version_ok=True, missing=[],
                    stale=['mc2.exe'] if broken == Path(target).name + '_identity' else [])
    monkeypatch.setattr(deploy_payload, 'staleness_report', published_bytes)
    for folder in ('candidate', 'seed'):
        bookmarks = tmp_path / folder / 'saved_bookmarks'
        bookmarks.mkdir(parents=True)
        if broken != 'camera' or folder != 'seed':
            pose = dict(pos=[1, 2, 3], rot=10, proj=20, alt=30)
            other = dict(pose, name='renamed', rot=10 if broken == 'duplicate' else 40)
            if broken == 'equivalent':
                other = dict(pose, name='renamed', pos=[1.0, 2.0, 3.0], rot=370.0)
            (bookmarks / 'day.json').write_text(json.dumps(dict(mission='m', bookmarks=[pose, other])))
    baseline = tmp_path / 'tests/saveload/baselines'
    baseline.mkdir(parents=True)
    (tmp_path / 'mask.json').write_text('{"across_builds": true}')
    import saveload_roundtrip
    monkeypatch.setattr(saveload_roundtrip, 'mask_policy', lambda path: {'mode': 'current'})
    (baseline / 'm.json').write_text(json.dumps(dict(comparison={'mode': 'old' if broken == 'policy' else 'current'},
        judged='first-save-only', overall='digest', runs=[{}], packets=1, save_at=1, load_at=2)))
    steps = [('capture-after', ['capture', '--bookmarks', 'day'], {}),
             ('saveload-m', ['save', '--mission', 'm', '--mask', 'mask.json'], {})]
    if scheduled_deploy:
        steps.append(('deploy', ['deploy'], {}))
    assert gate.preflight('a' * 40, tmp_path / 'candidate', 92,
                          steps, seed=tmp_path / 'seed') == int(broken is not None)
    output = capsys.readouterr().out
    assert checked == ['candidate', 'seed']
    assert 'fix:' in output if broken else 'preflight PASS' in output


def test_deploy_bytes_verified_before_runtime(tmp_path, monkeypatch):
    from types import SimpleNamespace
    import deploy_payload
    monkeypatch.setattr(gate, 'git', lambda *args: 'head')
    monkeypatch.setattr(gate, 'log_lease_holders', lambda: None)
    monkeypatch.setattr(gate, 'ACTIVE_STATE', None)
    checked = []
    def corrupt(target, root, head):
        checked.append((target, head))
        raise ValueError('shader byte mismatch')
    monkeypatch.setattr(deploy_payload, 'verify_published_identity', corrupt)
    monkeypatch.setattr(gate.gate_reuse, 'install_current', lambda *a: False)   # gate-reuse's pre-deploy probe: a miss, so the deploy runs
    calls = []
    def runner(command, **kw):
        calls.append(command)
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(gate, 'run_child', runner)
    monkeypatch.delenv('MC2_HOST_LEASE_BATCH', raising=False)
    command = ['python', 'scripts/deploy_payload.py', str(tmp_path), '--allow-new-target']
    assert gate.execute_parallel([('deploy', command, {}), ('tier1', ['runtime'], {})], 'head') == 1
    assert calls == [command] and checked == [(tmp_path, 'head')]

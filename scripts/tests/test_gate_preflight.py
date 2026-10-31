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
    (90, {'ammo': 7}, {}), (90, {'shape': 1}, {'shape': 1}),
    (90, {'ammo': 8}, {'ammo': 8}), (92, {'ammo': 7}, {'ammo': 7}),
    (88, {'ammo': 7}, {'ammo': 7}),
])
def test_known_failure_boundaries(window, counts, new):
    known, actual = mods.content_verdict(counts, 'MC2X-CVE-G', 'metropolitan', window,
                                       mods.load_known_failures())
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


@pytest.mark.parametrize('broken', [None, 'game', 'viewer', 'camera', 'policy', 'expiry'])
def test_preflight_inputs(tmp_path, monkeypatch, capsys, broken):
    monkeypatch.setattr(gate, 'ROOT', tmp_path)
    monkeypatch.setattr(exe_fingerprint, 'extract', lambda path: dict(
        present=True, sha='b' * 40 if path.name == {'game': 'mc2.exe', 'viewer': 'mc2_asset_viewer.exe'}.get(broken) else 'a' * 40, dirty=0))
    for folder in ('candidate', 'seed'):
        bookmarks = tmp_path / folder / 'saved_bookmarks'
        bookmarks.mkdir(parents=True)
        if broken != 'camera' or folder != 'seed':
            (bookmarks / 'day.json').write_text(json.dumps(dict(mission='m', bookmarks=[{}, {}])))
    baseline = tmp_path / 'tests/saveload/baselines'
    baseline.mkdir(parents=True)
    (tmp_path / 'mask.json').write_text('{"across_builds": true}')
    import saveload_roundtrip
    monkeypatch.setattr(saveload_roundtrip, 'mask_policy', lambda path: {'mode': 'current'})
    (baseline / 'm.json').write_text(json.dumps(dict(comparison={'mode': 'old' if broken == 'policy' else 'current'},
        judged='first-save-only', overall='digest', runs=[{}], packets=1, save_at=1, load_at=2)))
    steps = [('capture-after', ['capture', '--bookmarks', 'day'], {}),
             ('saveload-m', ['save', '--mission', 'm', '--mask', 'mask.json'], {})]
    assert gate.preflight('a' * 40, tmp_path / 'candidate', 92 if broken == 'expiry' else 90,
                          steps, seed=tmp_path / 'seed') == int(broken is not None)
    output = capsys.readouterr().out
    assert 'fix:' in output if broken else 'preflight PASS' in output

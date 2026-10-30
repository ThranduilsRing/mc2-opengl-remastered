"""Continuation briefs consume journals without launching a gate."""
import json
from pathlib import Path
import sys
import types

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mkmerge


@pytest.fixture
def window(tmp_path):
    build = tmp_path / 'build64-ninja'
    build.mkdir()
    window = dict(base='base', head='head', tips=['tip-one', 'tip-two'])
    state = dict(head='head', exe_sha='exe', artefact_dir='proof', order=['contracts', 'deploy', 'tier1'], stages={
        'contracts': dict(head='head', exe_sha='exe', status='REUSED: matched'),
        'deploy': dict(head='head', exe_sha='exe', status='FAIL: refusal: stale binary')})
    def save():
        (build / 'merge-window.json').write_text(json.dumps(window))
        (build / 'gate-state.json').write_text(json.dumps(state))
    save()
    return tmp_path, window, state, save


def test_continuation_reports_landed_next_and_refusals(window):
    root, _, _, _ = window
    assert mkmerge.main(['--continue', '86'], ops=root, root=root) == 0
    text = (root / 'sol-merge86-continue.md').read_text()
    assert 'tip-one' in text and 'tip-two' in text
    assert 'Next stage: deploy' in text and 'stale binary' in text
    assert '--resume' in text


@pytest.mark.parametrize('change', ['head', 'exe'])
def test_stale_stage_is_next(window, change):
    root, _, state, save = window
    state['stages']['contracts'][change if change == 'head' else 'exe_sha'] = 'old'
    save()
    assert 'Next stage: contracts' in mkmerge.continuation(86, root)


def test_mismatched_window_refuses_brief(window):
    root, _, state, save = window
    state['head'] = 'different'
    save()
    with pytest.raises(ValueError, match='HEAD differ'):
        mkmerge.main(['--continue', '86'], ops=root, root=root)
    assert not (root / 'sol-merge86-continue.md').exists()


def test_operator_omission_is_still_unfinished(window):
    root, _, state, save = window
    state['stages']['contracts']['status'] = 'SKIP: operator stage filter; partial gate'
    save()
    assert 'Next stage: contracts' in mkmerge.continuation(86, root)


def test_completed_window_and_legacy_tip_recovery(window, monkeypatch):
    root, record, state, save = window
    del record['tips']
    for name in state['order']:
        state['stages'][name] = dict(head='head', exe_sha='exe', status='PASS')
    save()
    seen = []
    def git(cmd, **kw):
        seen.append(cmd)
        return types.SimpleNamespace(stdout='parent landed-sha merge lane: useful change\n')
    monkeypatch.setattr(mkmerge.subprocess, 'run', git)
    text = mkmerge.continuation(86, root)
    assert 'landed-sha merge lane: useful change' in text
    assert 'none (gate stages complete)' in text and 'Open refusals:\n- none' in text
    assert '--first-parent' in seen[0]


def test_original_brief_interface_preserved(tmp_path):
    (tmp_path / 'sol-merge85.md').write_text('MERGE WINDOW 85\nTips, in order\nold\nRun every ratchet for abcdef0..HEAD\n')
    tips = tmp_path / 'tips.txt'
    tips.write_text('branch sha (description)\n')
    assert mkmerge.main(['86', '1234567', str(tips)], ops=tmp_path) == 0
    text = (tmp_path / 'sol-merge86.md').read_text()
    assert 'MERGE WINDOW 86' in text and '1. branch sha (description)' in text
    assert 'Run every ratchet for 1234567..HEAD' in text and 'HOST LEASE' in text

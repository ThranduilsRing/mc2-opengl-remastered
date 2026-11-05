"""Box route of the merge gate (box-gate stage, offload policy, Windows-full fallback), entirely offline."""
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import merge_gate as gate
import box_run

RUNTIME = ['code/mission_update.cpp']


def routed(paths=RUNTIME, full=False):
    steps = gate.affected_plan(gate.plan(6), paths, full)
    return {n: e for n, _, e in gate.apply_box_route(steps, full)}


def test_plan_starts_box_gate_first_with_the_gate_flag():
    name, command, _ = gate.plan(6)[0]
    assert name == 'box-gate'
    assert command[1:] == ['tools/box_run.py', '--gate']


def test_policy_reuses_the_offloaded_rows_and_keeps_the_msvc_canary():
    envs = routed()
    for row in gate.BOX_OFFLOADED:
        assert envs[row]['MC2_GATE_REUSE_STAGE'] == 'box-gate' and envs[row]['MC2_GATE_SKIP_REASON']
    for canary in ('determinism-mc2_10', 'saveload-mc2_10'):
        assert not envs[canary].get('MC2_GATE_SKIP_REASON')
    assert 'MC2_GATE_SKIP_REASON' not in envs['box-gate']


@pytest.mark.parametrize('full', [True])
def test_full_gate_keeps_every_row_on_the_pc(full):
    envs = routed(full=full)
    assert not any(e.get('MC2_GATE_REUSE_STAGE') == 'box-gate' for e in envs.values())


def test_skipped_box_stage_leaves_the_pc_rows_alone():
    steps = [(n, c, dict(e, MC2_GATE_SKIP_REASON='filtered') if n == 'box-gate' else e)
             for n, c, e in gate.affected_plan(gate.plan(6), RUNTIME)]
    assert not any(e.get('MC2_GATE_REUSE_STAGE') == 'box-gate' for _, _, e in gate.apply_box_route(steps))


def test_docs_only_diff_skips_the_box_stage_and_the_rows_together():
    envs = routed(['docs/readme.md'])
    assert envs['box-gate'].get('MC2_GATE_SKIP_REASON')
    assert not any(e.get('MC2_GATE_REUSE_STAGE') == 'box-gate' for e in envs.values())


def run_window(monkeypatch, tmp_path, box_rc, box_out='', fell_back_rows=None):
    ran = []

    def child(command, **kw):
        ran.append(command[0])
        if command[0] == 'box':
            return SimpleNamespace(returncode=box_rc, stdout=box_out)
        return SimpleNamespace(returncode=0, stdout='')
    monkeypatch.delenv('MC2_HOST_LEASE_BATCH', raising=False)
    monkeypatch.setattr(gate, 'git', lambda *a: 'head')
    monkeypatch.setattr(gate, 'log_lease_holders', lambda: None)
    monkeypatch.setattr(gate, 'run_child', child)
    monkeypatch.setattr(gate, 'ACTIVE_STATE', None)
    gate.FAILED_STAGES.clear()
    why = dict(MC2_GATE_SKIP_REASON='box', MC2_GATE_REUSE_STAGE='box-gate')
    steps = [('box-gate', ['box'], {}), ('deploy', ['deploy'], {}),
             ('determinism-mc2_10', ['det10'], {}), ('determinism-mc2_03', ['det03'], dict(why)),
             ('saveload-mc2_01', ['save01'], dict(why))]
    return gate.execute_parallel(steps, 'head'), ran


def test_green_box_stage_grades_the_offloaded_rows_reused(monkeypatch, tmp_path):
    result, ran = run_window(monkeypatch, tmp_path, 0)
    assert result == 0
    assert 'box' in ran and 'det03' not in ran and 'save01' not in ran and 'det10' in ran


def test_red_box_row_is_a_red_gate_and_the_pc_does_not_rerun_it(monkeypatch, tmp_path):
    result, ran = run_window(monkeypatch, tmp_path, 1, 'gate row determinism:mc2_03: FAIL(rc=1)')
    assert result == 1
    assert 'det03' not in ran and 'save01' not in ran
    assert {n for n, _ in gate.FAILED_STAGES} >= {'box-gate', 'determinism-mc2_03', 'saveload-mc2_01'}
    assert not gate.FAILED_STAGES[0][1].startswith('INFRA')


def test_box_infra_falls_back_to_the_windows_rows_loudly(monkeypatch, tmp_path, capsys):
    result, ran = run_window(monkeypatch, tmp_path, 3, 'BOX_INFRA: no verdict')
    assert result == 0
    assert 'det03' in ran and 'save01' in ran
    assert 'Windows-full fallback' in capsys.readouterr().out
    assert not gate.FAILED_STAGES


def test_probe_failure_selects_the_pc_fallback(monkeypatch):
    assert gate.box_reachable(lambda *a, **k: SimpleNamespace(returncode=0)) is True
    assert gate.box_reachable(lambda *a, **k: SimpleNamespace(returncode=255)) is False

    def boom(*a, **k):
        raise OSError('ssh missing')
    assert gate.box_reachable(boom) is False


def rec(*verdicts):
    return {'rows': [{'row': f'r{i}', 'verdict': v} for i, v in enumerate(verdicts)]}


def test_box_run_gate_verdict_codes(capsys):
    assert box_run.gate_verdict(rec('PASS', 'PASS')) == 0
    assert box_run.gate_verdict(rec('PASS', 'FAIL(rc=1)')) == 1
    assert box_run.gate_verdict(rec('PASS', 'NOT RUN')) == 3
    assert box_run.gate_verdict(rec()) == 3
    assert 'BOX_INFRA' in capsys.readouterr().out


def test_box_infra_marker_maps_to_an_infra_status():
    assert gate.infra_status(3, 'BOX_INFRA: x').startswith('INFRA (3)')


def test_route_is_default_on_and_opt_out_exists():
    import inspect
    src = inspect.getsource(gate.main)
    assert '"--box-gate", action=argparse.BooleanOptionalAction, default=True' in src

"""Box route of the merge gate (box-gate stage, offload policy, Windows-full fallback), and box_run --queue / --wait /
--status with the single-ssh run (ssh mocked: no box, no network); entirely offline."""
import base64
import io
import json
from pathlib import Path
import sys
import tarfile
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


# box_run --queue / --wait / --status (was scripts/tests/test_box_queue.py; folded here so the gate_farm row runs it).
def tasks(**by_id):
    return json.dumps({"tasks": by_id})


def test_rows_travel_as_one_base64_argument():
    assert base64.b64decode(box_run.rows_b64(["smoke-lite", "capture:mc2_01"])).decode() == "smoke-lite\ncapture:mc2_01"


def test_remote_script_truncates_the_append_only_tsvs_and_prints_lock_wait():
    assert 'rm -f "$OUT"/rows.tsv "$OUT"/fingerprints.tsv' in box_run.REMOTE
    assert "lock_wait" in box_run.REMOTE


@pytest.mark.parametrize("status,state,rc", [
    ({"Running": {"start": "x"}}, "Running", None),
    ("Queued", "Queued", None),
    ({"Done": {"result": "Success"}}, "Done", 0),
    ({"Done": {"result": {"Failed": 3}}}, "Done", 3),
])
def test_task_state_reads_pueue_status_json(monkeypatch, status, state, rc):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"7": {"status": status, "label": "box_run:b:abcd1234:gpu1"}}))
    assert box_run.task_state(7) == (state, rc, "box_run:b:abcd1234:gpu1")


def test_task_state_missing_task(monkeypatch):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks())
    assert box_run.task_state(9)[0] == "missing"


def test_status_prints_one_line_and_exit_code_2_while_running(monkeypatch, capsys, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"4": {"status": {"Running": {}}, "label": "box_run:b:abcd1234:gpu0"}}))
    assert box_run.task_verdict(4, tmp_path, wait=False) == 2
    assert capsys.readouterr().out.strip().splitlines() == ["[box_run] task=4 state=Running rc=-"]


def test_wait_blocks_fetches_reports_and_prints_one_verdict_line(monkeypatch, capsys, tmp_path):
    calls = []
    monkeypatch.setattr(box_run, "run", lambda cmd, **kw: calls.append(cmd[-1]))
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"5": {"status": {"Done": {"result": "Success"}}, "label": "box_run:br:abcd1234:gpu2"}}))

    def fake_fetch(out_remote, local_out):
        assert out_remote == "/workspace/lanes/br/out/abcd1234"
        (Path(local_out) / "rows.tsv").write_text("smoke-lite\t0\ncapture\t1\n")
    monkeypatch.setattr(box_run, "fetch_back", fake_fetch)
    assert box_run.task_verdict(5, tmp_path, wait=True) == 1   # capture rc=1 makes the run FAIL
    assert any("pueue wait 5" in c for c in calls)
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 1 and "state=Done rc=0 rows=1/2" in lines[0]


def test_failed_task_exits_1(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: tasks(**{"6": {"status": {"Done": {"result": {"Failed": 1}}}, "label": "x"}}))
    assert box_run.task_verdict(6, tmp_path, wait=False) == 1


def test_pick_gpu_takes_the_least_loaded(monkeypatch):
    monkeypatch.setattr(box_run, "ssh_out", lambda cmd: "0 2\n1 0\n2 1\n")
    assert box_run.pick_gpu() == "1"


def test_fetch_back_extracts_what_the_box_tarred(monkeypatch, tmp_path):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        data = b"smoke-lite\t0\n"
        ti = tarfile.TarInfo("./rows.tsv")
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(stdout=buf.getvalue(), returncode=0))
    box_run.fetch_back("/workspace/x", tmp_path)
    assert (tmp_path / "rows.tsv").read_text() == "smoke-lite\t0\n"
    assert "*.tsv" in box_run.BACK and "*.json" in box_run.BACK and "tail -n 200" in box_run.BACK


def queued_main(monkeypatch, tmp_path, argv, stdout="12\n"):
    sent = []

    def fake_run(cmd, **kw):
        sent.append((cmd[-1], kw.get("input")))
        return SimpleNamespace(returncode=0, stdout=stdout if "pueue add" in cmd[-1] else "", stderr="")
    monkeypatch.setattr(box_run, "run", fake_run)
    monkeypatch.setattr(box_run, "git", lambda *a, repo=None: ("br" if "--abbrev-ref" in a else "deadbeef" * 5) if a[0] == "rev-parse" else "")
    monkeypatch.setattr(box_run, "pick_gpu", lambda: "2")
    monkeypatch.setattr(box_run.subprocess, "run", lambda cmd, **kw: SimpleNamespace(stdout="deadbeef" * 5, returncode=0))
    monkeypatch.setattr(sys, "argv", ["box_run.py", "--repo", str(tmp_path), *argv])
    return box_run.main(), sent


def test_queue_submits_one_ssh_to_the_group_and_returns_the_task_id(monkeypatch, tmp_path, capsys):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gpu", "1", "determinism"])
    assert rc == 0
    pueue = [s for s in sent if "pueue add" in s[0] and "-g build" not in s[0]]   # the build-group task that precedes a GPU task is separate
    assert len(pueue) == 1
    cmd, stdin = pueue[0]
    assert "-g gpu1" in cmd and "--print-task-id" in cmd and stdin == box_run.REMOTE.encode()
    assert base64.b64encode(b"determinism").decode() in cmd
    assert "queued task=12 group=gpu1" in capsys.readouterr().out


def test_queue_gate_goes_to_the_gate_group(monkeypatch, tmp_path):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gate"])
    assert rc == 0 and any("-g gate" in s[0] and "--print-task-id" in s[0] for s in sent)


def test_blocking_mode_stays_the_default_one_ssh_run(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "fetch_back", lambda *a: None)
    rc, sent = queued_main(monkeypatch, tmp_path, ["--gpu", "0", "determinism"])
    assert not any("pueue" in s[0] for s in sent)
    assert sum("bash /workspace/lanes/.box_lane_br.sh" in s[0] for s in sent) == 1

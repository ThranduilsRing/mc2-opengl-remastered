"""box_run --queue / --wait / --status and the single-ssh run, with ssh mocked (no box, no network)."""
import base64
import io
import json
from pathlib import Path
import sys
import tarfile
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import box_run


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
    assert box_run.task_verdict(5, tmp_path, wait=True) == 0
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
    pueue = [s for s in sent if "pueue add" in s[0]]
    assert len(pueue) == 1
    cmd, stdin = pueue[0]
    assert "-g gpu1 --print-task-id" in cmd and stdin == box_run.REMOTE
    assert base64.b64encode(b"determinism").decode() in cmd
    assert "queued task=12 group=gpu1" in capsys.readouterr().out


def test_queue_gate_goes_to_the_gate_group(monkeypatch, tmp_path):
    rc, sent = queued_main(monkeypatch, tmp_path, ["--queue", "--gate"])
    assert rc == 0 and any("-g gate --print-task-id" in s[0] for s in sent)


def test_blocking_mode_stays_the_default_one_ssh_run(monkeypatch, tmp_path):
    monkeypatch.setattr(box_run, "fetch_back", lambda *a: None)
    rc, sent = queued_main(monkeypatch, tmp_path, ["--gpu", "0", "determinism"])
    assert not any("pueue" in s[0] for s in sent)
    assert sum("bash /workspace/lanes/.box_lane_br.sh" in s[0] for s in sent) == 1

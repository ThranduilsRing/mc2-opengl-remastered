"""Offline tests of merge gate journaling, exclusion and resume grading."""
import json
import os
from pathlib import Path
import subprocess
import sys
import types

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate


@pytest.fixture
def journal(tmp_path, monkeypatch):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"first exe")
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    gate.FAILED_STAGES.clear()
    return tmp_path / "gate-state.json", exe


def run_state(journal, monkeypatch, steps, runner, head="head", resume=False):
    path, exe = journal
    with gate.GateState(path, head, exe, path.parent / "proof", steps, resume) as state:
        monkeypatch.setattr(gate, "ACTIVE_STATE", state)
        result = gate.execute(steps, head, runner=runner)
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)
    return result, state


def test_stage_persisted_before_next_stage_and_resume_retries_failure(journal, monkeypatch, capsys):
    path, exe = journal
    steps = [("one", ["one"], {}), ("two", ["two"], {})]
    def runner(cmd, **kw):
        if cmd == ["two"]:
            row = json.loads(path.read_text())["stages"]["one"]
            assert row["head"] == "head" and row["exe_sha"]
            assert row["status"] == "PASS" and row["seconds"] >= 0
            assert row["artefact_dir"] == str(path.parent / "proof")
        return types.SimpleNamespace(returncode=int(cmd == ["two"]))
    assert run_state(journal, monkeypatch, steps, runner)[0] == 1
    seen = []
    def retry(cmd, **kw):
        seen.append(cmd)
        return types.SimpleNamespace(returncode=0)
    gate.FAILED_STAGES.clear()
    result, state = run_state(journal, monkeypatch, steps, retry, resume=True)
    assert result == 0 and seen == [["two"]] and state.reused == ["one"]
    assert not gate.FAILED_STAGES
    assert "REUSED: gate-state HEAD/exe/signature match" in capsys.readouterr().out


@pytest.mark.parametrize("change", ["head", "exe", "missing-exe", "command", "env", "fresh"])
def test_resume_invalidates_changed_evidence(journal, monkeypatch, change):
    steps = [("one", ["one"], {})]
    runner = lambda *a, **kw: types.SimpleNamespace(returncode=0)
    run_state(journal, monkeypatch, steps, runner)
    if change == "exe":
        journal[1].write_bytes(b"new exe")
    if change == "missing-exe":
        journal[1].unlink()
    if change == "command":
        steps = [("one", ["changed"], {})]
    if change == "env":
        steps = [("one", ["one"], {"AUDIT": "1"})]
    head = "new head" if change == "head" else "head"
    monkeypatch.setattr(gate, "git", lambda *a: head)
    result, state = run_state(journal, monkeypatch, steps, runner, head, change != "fresh")
    assert result == 0 and not state.reused


def test_dependency_reuse_accepts_resumed_pass_but_not_failed_source(journal, monkeypatch):
    source = ("source", ["source"], {})
    dependent = ("dependent", ["never-run"], {"MC2_GATE_SKIP_REASON": "covered", "MC2_GATE_REUSE_STAGE": "source"})
    runner = lambda *a, **kw: types.SimpleNamespace(returncode=0)
    run_state(journal, monkeypatch, [source], runner)
    gate.FAILED_STAGES.clear()
    result, state = run_state(journal, monkeypatch, [source, dependent], runner, resume=True)
    assert result == 0 and state.reused == ['source', 'dependent']
    assert not gate.FAILED_STAGES
    assert gate.execute([dependent], "head", completed={"source": "FAIL (1)"}) == 1
    assert gate.FAILED_STAGES == [("dependent", "FAIL reuse: source checks failed; covered")]


def test_state_refuses_second_live_process_and_releases_on_exit(journal):
    path, exe = journal
    with gate.GateState(path, "head", exe, path.parent, []) as state:
        assert json.loads(path.read_text())["pid"] == os.getpid()
        code = ("import sys; sys.path.insert(0, sys.argv[1]); from merge_gate import GateState; "
                "s=GateState(sys.argv[2], 'head', sys.argv[3], '.', []); s.__enter__()")
        result = subprocess.run([sys.executable, "-B", "-c", code, str(Path(gate.__file__).parent), str(path), str(exe)],
                                capture_output=True, text=True, **gate.console_child.child_kwargs(redirected=True))
        assert result.returncode != 0 and "live PID" in result.stderr
        assert json.loads(path.read_text())["pid"] == os.getpid()
    with gate.GateState(path, "head", exe, path.parent, []):
        pass


def test_dry_run_does_not_write_stage_evidence(journal, monkeypatch):
    path, exe = journal
    with gate.GateState(path, "head", exe, path.parent, []) as state:
        monkeypatch.setattr(gate, "ACTIVE_STATE", state)
        assert gate.execute([("one", ["one"], {})], "head", dry_run=True) == 0
        assert json.loads(path.read_text())["stages"] == {}
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)


def test_resume_preserves_original_log(tmp_path, monkeypatch):
    proof = tmp_path / 'proof'
    proof.mkdir()
    log = proof / 'gate.log'
    log.write_text('original evidence\n')
    monkeypatch.setattr(gate, 'ROOT', tmp_path)
    monkeypatch.setattr(gate, 'git', lambda *a: 'head')
    monkeypatch.setattr(gate, 'execute_parallel', lambda *a, **kw: 0)
    assert gate.main(['--dry-run', '--resume', '--proofs-dir', str(proof)]) == 0
    assert log.read_text().startswith('original evidence\n')

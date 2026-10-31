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


@pytest.mark.parametrize("change", ["head", "exe", "missing-exe", "command", "env"])
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
    result, state = run_state(journal, monkeypatch, steps, runner, head)
    assert result == 0 and not state.reused


@pytest.mark.parametrize('change', ['none', 'script', 'policy', 'head', 'failure', 'ambient'])
def test_offline_receipt_uses_head_and_input_hashes_without_resume(journal, monkeypatch, change):
    script = journal[0].parent / 'check.py'
    policy = journal[0].parent / 'policy.json'
    script.write_text('check v1')
    policy.write_text('{}')
    steps = [('contracts', [str(script), str(policy)], {})]
    first = lambda *a, **kw: types.SimpleNamespace(returncode=int(change == 'failure'), gate_elapsed=135.4)
    run_state(journal, monkeypatch, steps, first)
    if change == 'script':
        script.write_text('check v2')
    if change == 'policy':
        policy.write_text('{"ceiling": 1}')
    if change == 'ambient':
        monkeypatch.setenv('MC2_OPTIONS_OVERRIDE', 'RemasteredBuildings=FALSE')
    head = 'new head' if change == 'head' else 'head'
    monkeypatch.setattr(gate, 'git', lambda *a: head)
    # Offline proofs do not depend on the game executable's presence or bytes.
    journal[1].unlink()
    seen = []
    def child(*a, **kw):
        seen.append(a)
        return types.SimpleNamespace(returncode=0)
    result, state = run_state(journal, monkeypatch, steps, child, head=head)
    assert result == 0
    assert bool(seen) == (change != 'none')
    assert state.reused == (['contracts'] if change == 'none' else [])
    if change == 'none':
        assert state.data['stages']['contracts']['seconds'] == 135.4


@pytest.mark.parametrize('change', ['none', 'input', 'manifest', 'ambient'])
def test_runtime_reuses_only_identical_inputs_without_resume(journal, monkeypatch, change, capsys):
    policy = journal[0].parent / 'policy.json'
    policy.write_text('{}')
    install = journal[0].parent / 'install'
    install.mkdir()
    manifest = install / '.deployed_manifest.csv'
    manifest.write_text('payload v1')
    steps = [('saveload-mc2_03', ['check', '--mask', str(policy), '--install', str(install)], {})]
    child = lambda *a, **kw: types.SimpleNamespace(returncode=0)
    run_state(journal, monkeypatch, steps, child)
    if change == 'input':
        policy.write_text('{"changed": true}')
    if change == 'manifest':
        manifest.write_text('payload v2')
    if change == 'ambient':
        monkeypatch.setenv('MC2_SIM_OVERLAP_AUDIT', '1')
    result, state = run_state(journal, monkeypatch, steps, child)
    assert result == 0 and bool(state.reused) == (change == 'none')
    if change == 'none':
        row = state.data['stages']['saveload-mc2_03']
        assert f"REUSED saveload-mc2_03 sig={row['signature']} from {row['artefact_dir']}" in capsys.readouterr().out


def test_reopen_preserves_all_journal_keys(journal):
    path, exe = journal
    path.write_text(json.dumps({'stages': {}, 'started_signatures': {'one': 'sig'}}))
    # The second writer is constructed before the first acquires the OS lock.
    second = gate.GateState(path, 'head', exe, path.parent, [])
    with gate.GateState(path, 'head', exe, path.parent, []) as first:
        first.record('one', ['one'], {}, 'PASS', 1)
    with second:
        second.record('two', ['two'], {}, 'PASS', 1)
    saved = json.loads(path.read_text())
    assert set(saved['stages']) == {'one', 'two'}
    assert saved['started_signatures'] == {'one': 'sig'}


@pytest.mark.parametrize('change', ['none', 'head', 'mod', 'base', 'shader', 'proof', 'missing-proof', 'failed', 'ambient', 'candidate', 'during-run', 'stale-report', 'routing', 'engine-proof'])
def test_mod_receipt_replays_only_identical_inputs_and_retained_proof(journal, monkeypatch, change, capsys):
    import mod_smoke as mods
    import shutil
    root = journal[0].parent
    monkeypatch.setattr(gate, 'ROOT', root)
    base, mod_root, build, proof = (root / p for p in ('base', 'mods', 'build', 'proof'))
    files = [base / 'data/unit.fit', mod_root / 'one/data/mod.fit', build / 'shader.spv']
    for p in files:
        p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'initial')
    monkeypatch.setattr(mods, 'BATTERY', [('One', 'one', None)])
    monkeypatch.setattr(mods, 'engine_payload_items', lambda *a: [(files[2], 'shaders/vulkan/shader.spv')])
    output = root / 'artifacts'
    def steps(candidate):
        return [('mods', ['tools/mod_smoke.py', '--base-install', str(candidate), '--install-root', str(mod_root),
                          '--build-dir', str(build), '--stage-root', str(proof)], {'MC2_SMOKE_ARTIFACT_ROOT': str(output)})]
    def first(*a, **kw):
        if change == 'during-run':
            files[0].write_text(files[0].read_text() + 'changed')
        (proof / 'run').mkdir(parents=True, exist_ok=True)
        (proof / 'run/battery.json').write_text('[{"exit": 0}]')
        (proof / 'run/engine.log').write_text('PASS')
        output.mkdir(exist_ok=True)
        (output / 'mission.log').write_text('engine proof')
        return types.SimpleNamespace(returncode=int(change == 'failed'), gate_elapsed=489.3)
    if change == 'stale-report':
        first()
    run_state(journal, monkeypatch, steps(base), first)
    if change in ('mod', 'base', 'shader'):
        p = files[{'mod': 1, 'base': 0, 'shader': 2}[change]]
        stamp = p.stat()
        p.write_bytes(b'changed')
        os.utime(p, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    if change == 'proof':
        (proof / 'run/engine.log').write_text('changed evidence')
    if change == 'missing-proof':
        (proof / 'run/battery.json').unlink()
    if change == 'ambient':
        monkeypatch.setenv('MC2_OPTIONS_OVERRIDE', 'RemasteredBuildings=FALSE')
    if change == 'engine-proof':
        (output / 'mission.log').unlink()
    if change == 'routing':
        output = root / 'another-proof-root'
    if change == 'candidate':
        shutil.copytree(base, root / 'another-candidate')
        base = root / 'another-candidate'
    seen = []
    def child(*a, **kw):
        seen.append(a)
        return first(*a, **kw)
    head = 'changed' if change == 'head' else 'head'
    monkeypatch.setattr(gate, 'git', lambda *a: head)
    result, state = run_state(journal, monkeypatch, steps(base), child, head=head)
    assert result == int(change == 'failed')
    assert bool(state.reused) == (change in ('none', 'candidate', 'routing'))
    assert bool(seen) == (not state.reused)
    if state.reused:
        assert state.data['stages']['mods']['seconds'] == 489.3
        assert 'previous=489.3s' in capsys.readouterr().out


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
    monkeypatch.delenv('MC2_IN_GATE', raising=False)
    proof = tmp_path / 'proof'
    proof.mkdir()
    log = proof / 'gate.log'
    log.write_text('original evidence\n')
    monkeypatch.setattr(gate, 'ROOT', tmp_path)
    monkeypatch.setattr(gate, 'git', lambda *a: 'head')
    monkeypatch.setattr(gate, 'execute_parallel', lambda *a, **kw: 0)
    assert gate.main(['--dry-run', '--resume', '--proofs-dir', str(proof)]) == 0
    assert log.read_text().startswith('original evidence\n')

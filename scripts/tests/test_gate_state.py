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


def evidence(journal, name):
    """A runtime row's --out folder holding one proof file (runtime rows reuse only on recorded evidence)."""
    out = journal[0].parent / "evidence" / name
    out.mkdir(parents=True, exist_ok=True)
    (out / "run.log").write_text(f"{name} proof")
    return ["--out", str(out)]


def run_state(journal, monkeypatch, steps, runner, head="head", resume=False, planned=None):
    path, exe = journal
    with gate.GateState(path, head, exe, path.parent / "proof", steps if planned is None else planned, resume) as state:
        monkeypatch.setattr(gate, "ACTIVE_STATE", state)
        result = gate.execute(steps, head, runner=runner)
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)
    return result, state


def test_stage_persisted_before_next_stage_and_resume_retries_failure(journal, monkeypatch, capsys):
    path, exe = journal
    steps = [("one", ["one", *evidence(journal, "one")], {}), ("two", ["two", *evidence(journal, "two")], {})]
    def runner(cmd, **kw):
        if cmd[0] == "two":
            row = json.loads(path.read_text())["stages"]["one"]
            assert row["head"] == "head" and row["exe_sha"]
            assert row["status"] == "PASS" and row["seconds"] >= 0
            assert row["artefact_dir"] == str(path.parent / "evidence" / "one")  # the row's own --out
        return types.SimpleNamespace(returncode=int(cmd[0] == "two"))
    assert run_state(journal, monkeypatch, steps, runner)[0] == 1
    seen = []
    def retry(cmd, **kw):
        seen.append(cmd)
        return types.SimpleNamespace(returncode=0)
    gate.FAILED_STAGES.clear()
    result, state = run_state(journal, monkeypatch, steps, retry, resume=True)
    assert result == 0 and [c[0] for c in seen] == ["two"] and state.reused == ["one"]
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
    steps = [('saveload-mc2_03', ['check', '--mask', str(policy), '--install', str(install),
                                  *evidence(journal, 'saveload')], {})]
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


def test_concurrent_records_keep_both_stages_and_started_signatures(journal):
    from concurrent.futures import ThreadPoolExecutor
    import threading
    path, exe = journal
    barrier = threading.Barrier(2)
    with gate.GateState(path, 'head', exe, path.parent, []) as state:
        def record(name):
            state.started_signatures[name] = 'signature-' + name
            barrier.wait(timeout=5)
            state.record(name, [name], {}, 'PASS', 1)
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(record, ['one', 'two']))
        assert state.started_signatures == {'one': 'signature-one', 'two': 'signature-two'}
    assert set(json.loads(path.read_text())['stages']) == {'one', 'two'}


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
    assert result == 0   # a red mods row is ADVISORY (non-blocking, owner ruling 2026-11-08), never a gate failure
    if change == 'failed':
        assert state.data['stages']['mods']['status'].startswith('ADVISORY: FAIL')
    assert bool(state.reused) == (change in ('none', 'candidate', 'routing'))
    assert bool(seen) == (not state.reused)
    if state.reused:
        assert state.data['stages']['mods']['seconds'] == 489.3
        assert 'previous=489.3s' in capsys.readouterr().out


def test_dependency_reuse_accepts_resumed_pass_but_not_failed_source(journal, monkeypatch):
    source = ("source", ["source", *evidence(journal, "source")], {})
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


def test_determinism_retry_preserves_old_output_and_reuses_new_proof(journal, monkeypatch):
    output = journal[0].parent / 'night'
    (output / 'a').mkdir(parents=True)
    original = output / 'a' / 'engine.log'
    original.write_text('previous run')
    steps = [('determinism-mc2_10', ['tools/sim_determinism.py', 'check', 'mc2_10', '--out', str(output)], {})]
    seen = []
    def runner(command, **kwargs):
        actual = Path(command[command.index('--out') + 1])
        assert actual != output and not (actual / 'a').exists()
        (actual / 'a').mkdir()
        (actual / 'a' / 'engine.log').write_text('new proof')
        seen.append(actual)
        return types.SimpleNamespace(returncode=0)
    result, state = run_state(journal, monkeypatch, steps, runner)
    assert result == 0 and original.read_text() == 'previous run'
    assert steps[0][1][-1] == str(output)
    assert state.data['stages']['determinism-mc2_10']['artefact_dir'] == str(seen[0])
    result, state = run_state(journal, monkeypatch, steps, runner, resume=True)
    assert result == 0 and len(seen) == 1 and state.reused == ['determinism-mc2_10']


@pytest.mark.parametrize('kind', ['failed', 'passed', 'outside', 'shared'])
def test_explicit_cleanup_preserves_receipts_and_passing_evidence(journal, kind):
    path, exe = journal
    proof = path.parent / 'proof'
    output = (path.parent if kind == 'outside' else proof) / 'determinism-mc2_10'
    (output / 'a').mkdir(parents=True)
    (output / 'a/engine.log').write_text('retained unless failed')
    with gate.GateState(path, 'head', exe, proof, []) as state:
        state.data['stages']['determinism-mc2_10'] = dict(
            status='PASS' if kind == 'passed' else 'FAIL (1)', artefact_dir=str(output))
        if kind == 'shared':
            state.data['stages']['other'] = dict(status='PASS', artefact_dir=str(output / 'a'))
        state.write()
        receipt = path.read_bytes()
        if kind in ('outside', 'shared'):
            with pytest.raises(ValueError, match='refusing'):
                gate.clean_failed_outputs(state, proof)
        else:
            gate.clean_failed_outputs(state, proof)
        assert output.exists() == (kind != 'failed')
        assert path.read_bytes() == receipt


def test_cleanup_lock_does_not_rewrite_journal_metadata(journal):
    path, exe = journal
    original = dict(head='old-head', order=['contracts'], stages={})
    path.write_text(json.dumps(original))
    receipt = path.read_bytes()
    with gate.GateState(path, 'new-head', exe, path.parent, [], True, read_only=True) as state:
        gate.clean_failed_outputs(state, path.parent)
    assert path.read_bytes() == receipt


@pytest.mark.parametrize('change', ['none', 'candidate', 'tampered'])
def test_cached_deploy_requires_actual_candidate_identity(journal, monkeypatch, change):
    import deploy_payload
    root = journal[0].parent
    first, second = root / 'candidate-a', root / 'candidate-b'
    first.mkdir()
    second.mkdir()
    planned = [('deploy', ['scripts/deploy_payload.py', '--target', 'game'], {})]
    def steps(target):
        return [('deploy', planned[0][1] + [str(target), '--allow-new-target'], {})]
    def verify(target, source, head):
        if not (target / 'identity').is_file():
            raise ValueError('candidate payload is missing or stale')
    monkeypatch.setattr(deploy_payload, 'verify_published_identity', verify)
    seen = []
    def runner(command, **kwargs):
        target = Path(command[command.index('--allow-new-target') - 1])
        (target / 'identity').write_text('current payload')
        seen.append(target)
        return types.SimpleNamespace(returncode=0)
    assert run_state(journal, monkeypatch, steps(first), runner, planned=planned)[0] == 0
    if change == 'tampered':
        (first / 'identity').unlink()
    target = second if change == 'candidate' else first
    result, state = run_state(journal, monkeypatch, steps(target), runner, planned=planned)
    assert result == 0 and len(seen) == (1 if change == 'none' else 2)
    assert state.reused == (['deploy'] if change == 'none' else [])
    assert (target / 'identity').is_file()


@pytest.mark.parametrize("paths,ok", [
    (["docs/land-as-ready.md"], True),                       # doc
    (["tests/callers.json"], True),                          # test fixture / inventory
    (["scripts/tests/test_x.py"], True),                     # pytest file
    (["scripts/run_smoke.py"], False),                       # runtime driver: engine
    (["mclib/quad.cpp"], False),                             # C++
    (["shaders/a.frag", "docs/a.md"], False),                # any runtime path poisons the set
    (["data/missions/x.fit"], False),
    (["some/unknown.bin"], False),                           # unknown stays engine
    ([], False),                                             # no diff: not a delta we can vouch for
])
def test_stage_reuse_across_head_change_only_for_nonruntime_paths(monkeypatch, paths, ok):
    done = types.SimpleNamespace(stdout="\n".join(paths))
    monkeypatch.setattr(gate.subprocess, "run", lambda *a, **k: done)
    assert gate.head_delta_nonruntime("old", "new") is ok
    assert gate.head_delta_nonruntime("", "new") is False


@pytest.mark.parametrize("layout", ["out", "compare-only", "artifact-root", "none"])
def test_resume_refuses_row_whose_evidence_file_was_deleted(journal, monkeypatch, layout):
    """W116 item 4: a runtime row is reused only while every evidence file it recorded still hashes the same."""
    root = journal[0].parent
    a, b = root / "cap-a", root / "cap-b"
    for d in (a, b):
        d.mkdir()
        (d / "shot.png").write_bytes(d.name.encode())
    command, env, victim = {
        "out": (["ab", "--out", str(a)], {}, a / "shot.png"),
        "compare-only": (["ab", "--compare-only", str(a), str(b)], {}, b / "shot.png"),
        "artifact-root": (["smoke"], {"MC2_SMOKE_ARTIFACT_ROOT": str(a)}, a / "shot.png"),
        "none": (["smoke"], {}, None),
    }[layout]
    steps = [("visual", command, env)]
    runner = lambda *a, **kw: types.SimpleNamespace(returncode=0)
    run_state(journal, monkeypatch, steps, runner)
    if layout != "none":
        assert run_state(journal, monkeypatch, steps, runner, resume=True)[1].reused == ["visual"]
        victim.unlink()
    seen = []
    def again(*a, **kw):
        seen.append(a)
        return types.SimpleNamespace(returncode=0)
    result, state = run_state(journal, monkeypatch, steps, again, resume=True)
    assert result == 0 and not state.reused and seen


@pytest.mark.parametrize("outcome,expected", [
    ("winerror", "INFRA (1): WinError 32"), ("oserror", "INFRA: "), ("product", "FAIL (1)"), ("valueerror", "FAIL: ")])
def test_infra_reds_get_their_own_status(journal, monkeypatch, outcome, expected):
    """W116 item 7: a red row that failed on the host is INFRA, never a product FAIL (no suspect range)."""
    def runner(cmd, **kw):
        if outcome == "oserror":
            raise OSError("handle is invalid")
        if outcome == "valueerror":
            raise ValueError("bad capture")
        text = "[mod_smoke] PermissionError: [WinError 32] in use" if outcome == "winerror" else "[SMOKE v1] event=summary result=fail"
        return types.SimpleNamespace(returncode=1, stdout=text)
    gate.FAILED_STAGES.clear()
    assert run_state(journal, monkeypatch, [("visual", ["visual"], {})], runner)[0] == 1
    assert gate.FAILED_STAGES[0][1].startswith(expected)
    assert gate.stage_passed(gate.FAILED_STAGES[0][1]) is False
    assert gate.infra_status(0, "WinError 32") is None


@pytest.mark.parametrize("outcome,original", [("product", "FAIL (1)"), ("winerror", "INFRA (1): WinError 32")])
def test_red_or_infra_mods_row_is_advisory_not_blocking(journal, monkeypatch, capsys, tmp_path, outcome, original):
    """Owner ruling 2026-11-08: a red/INFRA mods-slim row never blocks the gate, and is never silently a PASS."""
    def runner(cmd, **kw):
        text = "[mod_smoke] PermissionError: [WinError 32] in use" if outcome == "winerror" else "boom"
        return types.SimpleNamespace(returncode=1, stdout=text)
    gate.FAILED_STAGES.clear()
    gate.ROW_RESULTS.clear()
    assert run_state(journal, monkeypatch, [("mods", ["mods"], {})], runner)[0] == 0
    assert gate.FAILED_STAGES == []
    status = gate.ROW_RESULTS["mods"]["status"]
    assert status == "ADVISORY: " + original and status != "PASS" and gate.stage_passed(status)
    assert "[merge_gate] ADVISORY mods" in capsys.readouterr().out
    steps = [("mods", [], {})]
    assert gate.verdict_of(0, steps) == ("PASS", [])
    out = tmp_path / "gate-verdict.json"
    gate.write_verdict([out], head="h", branch="b", verdict="PASS", steps=steps, skipped=[], artifacts={}, timings={}, cleanup_errors=[])
    assert json.loads(out.read_text())["advisory"] == [dict(name="mods", status="ADVISORY: " + original)]
    gate.ROW_RESULTS.clear()


def test_a_red_non_mods_row_still_blocks_and_a_green_mods_row_stays_pass():
    assert gate.advisory_status("visual", "FAIL (1)") == "FAIL (1)"
    assert gate.advisory_status("mods", "PASS") == "PASS"
    assert gate.advisory_status("mods", "SKIP: x") == "SKIP: x"


def test_contracts_host_infra_rows_make_a_named_pass_never_a_bare_pass(journal, monkeypatch):
    out = "  9 passed, 0 failed\n[HOST_INFRA_ROWS] 2: shadow_bake_init ktx2_valid\n"
    runner = lambda cmd, **kw: types.SimpleNamespace(returncode=0, stdout=out)
    gate.FAILED_STAGES.clear()
    gate.ROW_RESULTS.clear()
    assert run_state(journal, monkeypatch, [("contracts", ["contracts"], {})], runner)[0] == 0
    status = gate.ROW_RESULTS["contracts"]["status"]
    assert status == "PASS (host-INFRA rows: 2: shadow_bake_init ktx2_valid)" and gate.stage_passed(status)
    assert gate.host_infra_status("no marker", "PASS") == "PASS"
    assert gate.host_infra_status(out, "FAIL (1)") == "FAIL (1)"
    gate.ROW_RESULTS.clear()

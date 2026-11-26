"""Explicit installs must stage the baseline and reject reversed capture identities."""
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate
import exe_fingerprint


@pytest.mark.parametrize("commit", ["a" * 40, "b" * 40, None])
def test_nightly_published_head_skips_only_baseline_rows(monkeypatch, tmp_path, commit):
    head = "a" * 40
    monkeypatch.setattr(exe_fingerprint, "extract", lambda path: dict(present=bool(commit), sha=commit))
    names = ["capture-before", "capture-before-2", "capture-before-classic", "compare-captures",
             "compare-captures-classic", "compare-noise-2", "capture-after", "deploy", "determinism-mc2_01"]
    steps = [(n, ["capture", "--install", str(tmp_path)], {"MC2_GATE_BASE": "b" * 40}) for n in names]
    steps.append(("capture-before-3", [], {"MC2_GATE_SKIP_REASON": "already skipped"}))
    result = gate.nightly_capture_baseline(steps, head)
    for n, c, e in result[:-1]:
        skipped = commit == head and n.startswith(("capture-before", "compare-"))
        assert bool(e.get("MC2_GATE_SKIP_REASON")) == skipped
        assert e["MC2_GATE_BASE"] == "b" * 40
        if skipped:
            assert "no historical baseline payload" in e["MC2_GATE_SKIP_REASON"]
    assert result[-1][2]["MC2_GATE_SKIP_REASON"] == "already skipped"
    assert "MC2_GATE_SKIP_REASON" not in steps[0][2]
    if commit == head:
        staged = []
        monkeypatch.setattr(gate, "stage_published", lambda install, swap_payload: staged.append(swap_payload))
        monkeypatch.setattr(gate, "execute_parallel", lambda *a, **k: 0)
        assert gate.execute_window(result, head, install=tmp_path) == 0
        assert staged == [False]


@pytest.mark.parametrize("capture", [True, False])
def test_explicit_install_stages_published_before_capture(monkeypatch, tmp_path, capture):
    events = []
    steps = [("capture-before" if capture else "deploy", ["capture", "--install", str(tmp_path)], {})]
    monkeypatch.setattr(gate, "stage_published", lambda install, swap_payload: events.append(("stage", install, swap_payload)))
    monkeypatch.setattr(gate, "execute_parallel", lambda *a, **k: events.append(("execute", a[0])) or 0)
    assert gate.execute_window(steps, "a" * 40, install=tmp_path) == 0
    assert events == [("stage", tmp_path, capture), ("execute", steps)]


@pytest.mark.parametrize("name,commit,passes", [
    ("capture-before", "a" * 40, False),
    ("capture-after", "b" * 40, False),
    ("capture-before", "b" * 40, True),
    ("capture-after", "a" * 40, True),
    ("capture-before", None, False),
    ("capture-after", None, False),
])
def test_capture_identity_fails_closed(monkeypatch, tmp_path, name, commit, passes):
    monkeypatch.setattr(exe_fingerprint, "extract", lambda path: dict(present=bool(commit), sha=commit))
    command = ["capture", "--install", str(tmp_path)]
    if passes:
        gate.check_capture_commit(name, command, "a" * 40)
    else:
        monkeypatch.setattr(gate, "git", lambda *args: "a" * 40)
        monkeypatch.setattr(gate, "ACTIVE_STATE", None)
        assert gate.execute([(name, command, {})], "a" * 40,
                            runner=lambda *a, **k: pytest.fail("bad identity launched")) == 1
        assert gate.ROW_RESULTS[name]["status"].startswith("FAIL:")

"""Explicit installs must stage the baseline and reject reversed capture identities."""
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate
import exe_fingerprint


def test_explicit_install_stages_published_before_capture(monkeypatch, tmp_path):
    events = []
    steps = [("capture-before", ["capture", "--install", str(tmp_path)], {})]
    monkeypatch.setattr(gate, "stage_published", lambda install: events.append(("stage", install)))
    monkeypatch.setattr(gate, "execute_parallel", lambda *a, **k: events.append(("execute", a[0])) or 0)
    assert gate.execute_window(steps, "a" * 40, install=tmp_path) == 0
    assert events == [("stage", tmp_path), ("execute", steps)]


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

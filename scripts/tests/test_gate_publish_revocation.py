"""publish() honours a newer failing --isolated-verdict run at the same HEAD (Sol P1: green window, red nightly, publish)."""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate

HEAD = "a" * 40


def setup(tmp_path, monkeypatch, green_at=100.0):
    (tmp_path / "build64-ninja").mkdir()
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    (tmp_path / gate.GREEN_MARK).write_text(json.dumps(dict(head=HEAD, finished=green_at)))
    ran = []
    monkeypatch.setattr(gate, "execute", lambda steps, head, dry_run=False: ran.append(steps[0][0]) or 0)
    monkeypatch.setattr(gate, "drop_candidate", lambda: None)
    monkeypatch.setattr(gate, "plan", lambda *a, **k: [("deploy", ["deploy"], {})])
    return ran


def isolated(tmp_path, monkeypatch, verdict, at, head=HEAD):
    monkeypatch.setattr(gate.time, "time", lambda: at)
    gate.record_isolated_verdict(head, verdict)


def test_green_window_then_red_nightly_refuses_publish(tmp_path, monkeypatch, capsys):
    ran = setup(tmp_path, monkeypatch)
    isolated(tmp_path, monkeypatch, "FAIL", 200.0)
    assert gate.publish(HEAD) == 1 and not ran
    assert "newer isolated gate run" in capsys.readouterr().out
    # the green marker itself is untouched (separate verdict files)
    assert json.loads((tmp_path / gate.GREEN_MARK).read_text())["head"] == HEAD


def test_green_window_without_nightly_publishes(tmp_path, monkeypatch):
    ran = setup(tmp_path, monkeypatch)
    assert gate.publish(HEAD) == 0 and ran == ["publish-v0.4"]


def test_passing_nightly_or_other_head_or_older_red_does_not_block(tmp_path, monkeypatch):
    ran = setup(tmp_path, monkeypatch)
    isolated(tmp_path, monkeypatch, "PASS", 200.0)
    assert gate.publish(HEAD) == 0
    isolated(tmp_path, monkeypatch, "FAIL", 200.0, head="b" * 40)
    assert gate.publish(HEAD) == 0
    isolated(tmp_path, monkeypatch, "FAIL", 50.0)   # older than the green window
    assert gate.publish(HEAD) == 0 and len(ran) == 3


def test_partial_nightly_also_revokes(tmp_path, monkeypatch):
    ran = setup(tmp_path, monkeypatch)
    isolated(tmp_path, monkeypatch, "PARTIAL", 200.0)
    assert gate.publish(HEAD) == 1 and not ran

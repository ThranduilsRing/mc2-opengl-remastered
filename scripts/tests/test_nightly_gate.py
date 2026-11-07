import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nightly_gate  # noqa: E402
import lane_merge_stage  # noqa: E402


def test_refuses_off_nifty_or_dirty(monkeypatch):
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "other" if a[0] == "branch" else "")
    assert "not on" in nightly_gate.refusal()
    monkeypatch.setattr(nightly_gate, "git", lambda *a: nightly_gate.NIFTY if a[0] == "branch" else " M x")
    assert nightly_gate.refusal() == "mc2-main is dirty"
    monkeypatch.setattr(nightly_gate, "git", lambda *a: nightly_gate.NIFTY if a[0] == "branch" else "")
    assert nightly_gate.refusal() is None


def test_skips_while_quiet_marker_live(monkeypatch, capsys):
    monkeypatch.setattr(nightly_gate, "refusal", lambda: None)
    monkeypatch.setattr(nightly_gate, "quiet_held", lambda path=None: True)
    assert nightly_gate.main([]) == 0
    assert "SKIP" in capsys.readouterr().out


def test_stage_selection_is_tier1_plus_deploy():
    names = nightly_gate.stage_names(False)
    assert "deploy" in names and "player-path" in names and "mods" not in names
    assert "mods" in nightly_gate.stage_names(True)


def test_promote_refuses_untriaged_nightly_fail(tmp_path):
    d = tmp_path / "nightly-2026-11-06"
    d.mkdir()
    (d / "gate-verdict.json").write_text(json.dumps({"verdict": "FAIL"}))
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path))
    (d / "triage.md").write_text("known")
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path)) == []
    e = tmp_path / "nightly-2026-11-07"
    e.mkdir()
    (e / "gate-verdict.json").write_text(json.dumps({"verdict": "PASS"}))
    (d / "triage.md").unlink()
    assert lane_merge_stage.untriaged_nightly_fail(str(tmp_path)) == []

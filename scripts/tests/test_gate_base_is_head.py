"""A change-based gate run whose base is HEAD compares a commit with itself and passes vacuously: refuse it."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_gate as gate


def test_base_equal_head_without_full_is_refused(monkeypatch, capsys):
    head = gate.git("rev-parse", "HEAD")
    monkeypatch.setattr(sys, "argv", ["merge_gate.py", "--dry-run", "--base", head])
    with pytest.raises(SystemExit) as exc:
        gate.main()
    assert exc.value.code == 2
    assert "equals HEAD" in capsys.readouterr().err



@pytest.mark.parametrize("nightly,explicit,expected", [(True, None, "window"), (True, "explicit", "explicit"),
                                                     (False, None, "window")])
def test_default_base_uses_verified_window(tmp_path, monkeypatch, capsys, nightly, explicit, expected):
    import json
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    (tmp_path / "build64-ninja").mkdir()
    (tmp_path / "build64-ninja/merge-window.json").write_text(json.dumps(
        {"head": "head", "verify": "OK", "base": "window"}))
    monkeypatch.setattr(gate, "plan", lambda *a: [("contracts", [], {})])
    def git(*args):
        if args[0] == "diff":
            assert args[-2:] == (expected, "head")
            raise RuntimeError("base selected")
        return "head"
    monkeypatch.setattr(gate, "git", git)
    argv = ["--dry-run"] + (["--nightly"] if nightly else []) + (["--base", explicit] if explicit else [])
    with pytest.raises(RuntimeError, match="base selected"):
        gate.main(argv)

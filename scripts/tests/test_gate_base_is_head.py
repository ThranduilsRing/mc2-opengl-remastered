"""A change-based gate run whose base is HEAD compares a commit with itself and passes vacuously: refuse it."""
import sys
import subprocess
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


def test_nightly_base_uses_newest_ancestor_tag(tmp_path, monkeypatch):
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    def git(*args):
        return subprocess.check_output(["git", "-c", "user.name=Test", "-c", "user.email=test@local",
                                        *args], cwd=tmp_path, text=True,
                                       **gate.console_child.child_kwargs(redirected=True)).strip()
    git("init")
    git("commit", "--allow-empty", "-m", "base")
    git("tag", "promote-old")
    old = git("rev-parse", "HEAD")
    git("checkout", "-b", "unrelated")
    git("commit", "--allow-empty", "-m", "unrelated")
    git("tag", "promote-unrelated")
    git("checkout", "-B", "lane", old)
    git("commit", "--allow-empty", "-m", "promoted")
    monkeypatch.setenv("GIT_COMMITTER_DATE", "2030-01-01T00:00:00Z")
    git("tag", "-a", "promote-new", "-m", "promoted")
    promoted = git("rev-parse", "HEAD")
    git("commit", "--allow-empty", "-m", "head")
    git("tag", "promote-head")
    head = git("rev-parse", "HEAD")
    assert gate.nightly_base(head) == ("promote-new", promoted)
    git("tag", "-d", "promote-old", "promote-new")
    with pytest.raises(ValueError, match="strict ancestor"):
        gate.nightly_base(head)


@pytest.mark.parametrize("nightly,explicit,expected", [(True, None, "promoted"), (True, "explicit", "explicit"),
                                                     (False, None, "window")])
def test_nightly_default_is_printed_and_overrides_window(tmp_path, monkeypatch, capsys, nightly, explicit, expected):
    import json
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    (tmp_path / "build64-ninja").mkdir()
    (tmp_path / "build64-ninja/merge-window.json").write_text(json.dumps(
        {"head": "head", "verify": "OK", "base": "window"}))
    monkeypatch.setattr(gate, "plan", lambda *a: [("contracts", [], {})])
    monkeypatch.setattr(gate, "nightly_base", lambda head: ("promote-test", "promoted"))
    def git(*args):
        if args[0] == "diff":
            assert args[-2:] == (expected, "head")
            raise RuntimeError("base selected")
        return "head"
    monkeypatch.setattr(gate, "git", git)
    argv = ["--dry-run"] + (["--nightly"] if nightly else []) + (["--base", explicit] if explicit else [])
    with pytest.raises(RuntimeError, match="base selected"):
        gate.main(argv)
    assert ("promote-test promoted" in capsys.readouterr().out) == (nightly and not explicit)

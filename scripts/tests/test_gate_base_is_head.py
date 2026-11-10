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

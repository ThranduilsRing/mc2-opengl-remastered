"""run_scripts_tests runs any scripts/tests/test_*.py by bare name, file name or path (not only by the full repo-relative path)."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import run_scripts_tests as rst  # noqa: E402

MODULES = ["scripts/tests/test_a.py", "scripts/tests/test_b.py"]
DATA = {"aggregate_owner": "scripts_tests", "tests": [{"module": "scripts/tests/test_b.py", "owners": ["direct"]}]}


@pytest.mark.parametrize("name", ["test_b", "test_b.py", "scripts/tests/test_b.py", r"scripts\tests\test_b.py"])
def test_module_is_selectable_by_any_spelling(name):
    assert rst.select_modules(MODULES, DATA, [name]) == ["scripts/tests/test_b.py"]


def test_unknown_module_still_refused():
    with pytest.raises(ValueError, match="unknown test module: test_zzz"):
        rst.select_modules(MODULES, DATA, ["test_zzz"])


def test_direct_owner_is_excluded_only_when_it_runs():
    assert rst.select_modules(MODULES, DATA) == MODULES[:1]
    assert rst.select_modules(MODULES, DATA, active_owners={"direct"}) == MODULES[:1]
    assert rst.select_modules(MODULES, DATA, active_owners=set()) == MODULES


def test_failed_module_is_not_rerun(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["runner", "test_a"])
    monkeypatch.setattr(rst, "discover", lambda: MODULES)
    monkeypatch.setattr(rst, "load_ownership", lambda: DATA)
    monkeypatch.setattr(rst.rtt, "run_all", lambda *a: [(MODULES[0], 1, ["test_a::failed"], "first failure")])
    monkeypatch.setattr(rst.subprocess, "run", lambda *a, **k: pytest.fail("failure rerun"))
    assert rst.main() == 1
    assert "first failure" in capsys.readouterr().out

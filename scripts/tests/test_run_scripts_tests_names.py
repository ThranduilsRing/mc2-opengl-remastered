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

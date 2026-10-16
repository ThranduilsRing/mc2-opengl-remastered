"""Tests for tools/journal_comment_ratchet.py (in-memory fixtures; stdlib only).

Run:
  py -3 -B -m pytest scripts/tests/test_journal_comment_ratchet.py -q -p no:cacheprovider
"""
import importlib.util
import io
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
_spec = importlib.util.spec_from_file_location("journal_comment_ratchet", os.path.join(ROOT, "tools", "journal_comment_ratchet.py"))
jcr = importlib.util.module_from_spec(_spec)
sys.modules["journal_comment_ratchet"] = jcr
_spec.loader.exec_module(jcr)


def n(rel, src):
    return len(jcr.matches(rel, src))


@pytest.mark.parametrize("rel,src", [
    ("a.cpp", "int x; // BRAIN-MISSIONFIT-OPORD-CONSUMER-1: consume the order\n"),
    ("a.cpp", "/* S4-b2a authored route */\n"),
    ("a.h", "/*\n * keeps the old path (Phase 3)\n */\n"),
    ("a.frag", "// Slice M fallback\n"),
    ("a.comp", "// added in rev7\n"),
    ("a.cpp", "// Step 4 of the plan\n"),
    ("a.cpp", "// Astra correction 5 applied\n"),
    ("a.cpp", "// review correction: clamp\n"),
    ("a.cpp", "// see docs/superpowers/plans/foo.md\n"),
    ("a.py", "x = 1  # OPORD-ROUTE-2 tag\n"),
])
def test_journal_comment_counted(rel, src):
    assert n(rel, src) == 1


@pytest.mark.parametrize("rel,src", [
    ("a.cpp", "// UTF-8 text, SSE-2 path, IEEE-754 float, C++23, VK_KHR_swapchain, 0xDEADBEEF, CVE-2021-1234\n"),
    ("a.cpp", "// clamp to the near plane so the shadow cascade does not pop\n"),
    ("a.cpp", 'const char* s = "BRAIN-OPORD-CONSUMER-1"; int phase3 = 0;\n'),   # string literal, not a comment
    ("a.py", 's = "S4-b2a Phase 3"\n"""BRAIN-OPORD-1 docstring"""\n'),           # string and docstring, not comments
    ("a.cpp", "// X-1 offset and a revolution count\n"),
])
def test_plain_comment_not_flagged(rel, src):
    assert n(rel, src) == 0


def test_lines_counted_once_each():
    assert n("a.cpp", "/* AA-BB-1\n BB-CC-2 and Phase 2\n plain */\n") == 2


def test_check_gates_on_total_only():
    base = {"files": {"a.cpp": 2}}
    assert jcr.check({"a.cpp": 2}, base, io.StringIO()) == 0
    assert jcr.check({"a.cpp": 1}, base, io.StringIO()) == 0
    assert jcr.check({"a.cpp": 1, "moved.cpp": 1}, base, io.StringIO()) == 0     # pure move
    out = io.StringIO()
    assert jcr.check({"a.cpp": 3}, base, out) == 1                                 # one added tag
    assert "what/why instead of the plan/slice tag" in out.getvalue() and "a.cpp (2 -> 3)" in out.getvalue()
    assert jcr.check({"a.cpp": 1, "new.cpp": 2}, base, io.StringIO()) == 1

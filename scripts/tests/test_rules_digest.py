"""tools/rules_digest.py: deterministic, <= 60 lines, footer, and no must/never rule silently dropped."""
from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import rules_digest as rd  # noqa: E402

RULES = rd.DEFAULT_RULES
pytestmark = pytest.mark.skipif(not RULES.exists(), reason="ops lane-rules.md not on this machine")

MUST = re.compile(r"\b(never|must|do not|don't)\b", re.I)


def test_deterministic():
    assert rd.generate(RULES) == rd.generate(RULES)


def test_shape():
    for typ, body in rd.generate(RULES).items():
        lines = body.rstrip("\n").split("\n")
        assert len(lines) <= rd.MAX_LINES, typ
        assert lines[-1] == rd.FOOTER, typ


def test_every_must_never_line_in_some_digest():
    digests = "\n".join(rd.generate(RULES).values()).split("\n")
    present = set(digests)
    missing = []
    for _sec, line in rd.parse(RULES.read_text(encoding="utf-8")):
        if MUST.search(line) and line not in present:
            missing.append(line[:90])
    assert not missing, missing


def test_section7_prefixes_all_match():
    # A renamed bullet in lane-rules must not make a SECTION7 selector dead.
    items = [line for sec, line in rd.parse(RULES.read_text(encoding="utf-8")) if sec == "7"]
    for prefix, _types in rd.SECTION7:
        assert any((l[2:] if l.startswith("- ") else l).startswith(prefix) for l in items), prefix


def test_check_flag(tmp_path):
    assert rd.main(["--out-dir", str(tmp_path), "--check"]) == 1
    assert rd.main(["--out-dir", str(tmp_path)]) == 0
    assert rd.main(["--out-dir", str(tmp_path), "--check"]) == 0

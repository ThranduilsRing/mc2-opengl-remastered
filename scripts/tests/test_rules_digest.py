"""tools/rules_digest.py: deterministic, verbatim selection, headings + pointers, absolute footer, byte budget, --check."""
from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import rules_digest as rd  # noqa: E402

RULES = rd.DEFAULT_RULES
needs_rules = pytest.mark.skipif(not RULES.exists(), reason="ops lane-rules.md not on this machine")

SAMPLE = """# Title
Prose line about JOBS and -jN.

## Owner rules (x)
- Never push.
- Never temporal.

## 7. Proof (overrides any older line)
- Run the rung.

| Change | Proof |
|---|---|
| Docs | diff |
| Sim-visible C++ | det |
- PC compile: one.
"""


def _lines(text):
    return [l.rstrip() for l in text.splitlines() if l.strip()]


@needs_rules
def test_deterministic():
    assert rd.generate(RULES) == rd.generate(RULES)


@needs_rules
def test_every_rule_line_verbatim_or_pointed_to_per_digest():
    pre, items = rd.parse(RULES.read_text(encoding="utf-8"))
    assert any("-jN" in l for l in pre), "prose (JOBS/-jN definition) must be parsed"
    for typ, body in rd.generate(RULES).items():
        present = set(_lines(body))
        for l in pre:
            assert l in present, (typ, l[:80])
        for sec, head, line in items:
            if line in present:
                continue
            assert rd.pointer(sec, head) in present, (typ, sec, line[:80])


@needs_rules
def test_section_headings_kept_so_section_refs_resolve():
    heads = [l for l in RULES.read_text(encoding="utf-8").splitlines() if l.startswith("## ")]
    sec7 = next(h for h in heads if h.startswith("## 7."))
    assert "overrides any older line in a brief" in sec7
    for typ, body in rd.generate(RULES).items():
        present = set(_lines(body))
        assert sec7 in present, typ
        rules_text = "\n".join(l for l in present if not l.startswith("see "))
        for ref in set(re.findall(r"section (\d+)", rules_text)):
            h = next(x for x in heads if x.startswith(f"## {ref}."))
            assert h in present or any(f"section {ref} (" in l for l in present), (typ, ref)


@needs_rules
def test_footer_is_absolute_and_exists():
    for typ, body in rd.generate(RULES).items():
        last = _lines(body)[-1]
        assert last == rd.FOOTER, typ
        path = re.search(r"full rules: (\S+?);", last).group(1)
        assert Path(path).is_absolute() and Path(path).exists()


@needs_rules
def test_byte_budget_enforced():
    digests = rd.generate(RULES)
    assert rd.over_budget(digests) == []
    assert rd.BUDGET[rd.E] <= 8300   # engine digest stays small; it carries the Docs/Tooling rows (engine lanes edit Python tools)
    ls = _lines(digests[rd.E])
    assert any(l.startswith("| Docs |") for l in ls) and any(l.startswith("| Tooling") for l in ls)


@needs_rules
def test_drop_prefixes_all_match():
    # A renamed bullet in lane-rules must not leave a dead DROP selector.
    _pre, items = rd.parse(RULES.read_text(encoding="utf-8"))
    for typ, drops in rd.DROP.items():
        for sec, prefix in drops:
            assert any(s == sec and rd._body(l).startswith(prefix) for s, _h, l in items), (typ, sec, prefix)


@needs_rules
def test_head_line_marked_generated():
    for typ, body in rd.generate(RULES).items():
        assert _lines(body)[1].startswith("Generated header (not a rule)"), typ


def test_over_budget_is_reported_not_raised(monkeypatch):
    monkeypatch.setitem(rd.BUDGET, rd.T, 10)
    pre, items = rd.parse(SAMPLE)
    digests = {t: rd.render(pre, items, t) for t in rd.TYPES}
    msgs = rd.over_budget(digests)
    assert msgs and "OVER BUDGET: digest-tools-only.md" in msgs[0] and "budget 10" in msgs[0]


def test_sample_selection_keeps_prose_headings_and_points_at_dropped(monkeypatch):
    monkeypatch.setitem(rd.DROP, rd.M, [("7", "| Sim-visible")])
    pre, items = rd.parse(SAMPLE)
    out = rd.render(pre, items, rd.M)
    assert "Prose line about JOBS and -jN." in out
    assert "## 7. Proof (overrides any older line)" in out
    assert "| Sim-visible C++ | det |" not in out and f"see {rd.RULES_ABS} section 7 (Proof)" in out
    assert "- Run the rung." in out and "- Never temporal." in out


@needs_rules
def test_check_flag(tmp_path):
    assert rd.main(["--out-dir", str(tmp_path), "--check"]) == 1   # missing files are stale
    assert rd.main(["--out-dir", str(tmp_path)]) == 0
    assert rd.main(["--out-dir", str(tmp_path), "--check"]) == 0
    f = tmp_path / "digest-engine.md"
    f.write_text(f.read_text(encoding="utf-8") + "extra\n", encoding="utf-8")
    assert rd.main(["--out-dir", str(tmp_path), "--check"]) == 1   # stale file detected

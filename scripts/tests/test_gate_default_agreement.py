"""Tests for the doc sweep of scripts/check-gate-default-agreement.py (rule 3).

The sweep must stay LIVE for every ordinary doc, but a generated doc that quotes old
registry prose verbatim (docs/gate-history.md) is history, not a current claim, and
must be skipped -- via the shared marker, not a path allowlist, so the exemption is
visible in the file itself.
"""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import gate_idioms as gi  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "check_gate_default_agreement",
    os.path.join(ROOT, "scripts", "check-gate-default-agreement.py"))
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)

CLAIM_LINE = "MC2_SOME_GATE: Default-off; =1 enables.\n"


def _claims(tmp_path, files):
    docs = tmp_path / "docs"
    docs.mkdir()
    for name, text in files.items():
        (docs / name).write_text(text, encoding="utf-8")
    return chk.load_comment_claims(root=str(tmp_path), scan_dirs=[], claim_dirs=[],
                                   exclude=())


def test_ordinary_doc_default_claims_are_still_swept(tmp_path):
    claims = _claims(tmp_path, {"plain.md": "# Notes\n\n" + CLAIM_LINE})
    assert [(c["var"], c["path"]) for c in claims] == [("MC2_SOME_GATE", "docs/plain.md")]


def test_doc_carrying_the_quoted_history_marker_is_skipped(tmp_path):
    text = "# Old\n\n" + gi.QUOTED_HISTORY_MARKER + "\n\n" + CLAIM_LINE
    assert _claims(tmp_path, {"hist.md": text}) == []


def test_marker_far_below_the_head_does_not_exempt_the_doc(tmp_path):
    # A stray marker deep in a doc must not silence the claims above it.
    text = "# Doc\n" + "filler\n" * 30 + CLAIM_LINE + gi.QUOTED_HISTORY_MARKER + "\n"
    claims = _claims(tmp_path, {"late.md": text})
    assert [c["var"] for c in claims] == ["MC2_SOME_GATE"]


def test_generated_gate_history_is_exempt_and_its_claims_do_not_surface(tmp_path):
    md = ("# Gate history\n" + gi.QUOTED_HISTORY_MARKER + "\n\n## MC2_OLD\n\n"
          "MC2_OLD default-off; =1 enables. Second sentence.\n")
    assert "default-off" in md                      # the verbatim history is really there
    assert _claims(tmp_path, {"gate-history.md": md}) == []


def test_graded_floor_catches_a_table_that_lost_its_bool_rows(monkeypatch):
    # Claims are still swept, but nothing matches a Bool row: the comparison graded nothing and must not pass.
    monkeypatch.setattr(chk, "load_defaults", lambda *a, **k: {})
    assert chk.run(quiet=True) == 1


def test_real_tree_grades_above_the_floor():
    assert chk.run(quiet=True) == 0

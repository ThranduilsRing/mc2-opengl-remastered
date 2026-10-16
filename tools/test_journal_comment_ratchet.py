"""Tests for tools/journal_comment_ratchet.py (pattern table, comment lexing, the added-lines diff mode on a throwaway git repo).

Run:
  py -3 -B -m pytest tools/test_journal_comment_ratchet.py -q -p no:cacheprovider
"""
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402
import journal_comment_ratchet as j  # noqa: E402


@pytest.mark.parametrize("comment,pattern", [
    ("// converted in step 4 batch B2.4", "batch-tag"),
    ("// B3.1b: new behaviour", "batch-tag"),
    ("// Slice A: cement overlay bridge", "step-slice-phase"),
    ("// see .claude/cleanup/cxx23-step4-b1-notes.md section 5", "plan-doc"),
    ("// BUGS row 21", "plan-doc"),
    ("// RB2 decided the heap rules", "review-id"),
    ("// the old failure (H18)", "review-id"),
    ("// review F3 asked for this", "review-id"),
    ("// Codex owns this file", "agent-name"),
    ("// lane 2 claim", "agent-name"),
    ("// TERRAIN-NORMALS-FROM-HEIGHT-1: normals from the height field", "arc-id"),
])
def test_journal_patterns_hit(comment, pattern):
    assert pattern in j.hits_in_comment(comment)


@pytest.mark.parametrize("comment", [
    "// WHAT: equal scores keep their fill order. WHY: the sim must not depend on the library sort.",
    "// FNV-1a over the bytes; UTF-8 names stay byte strings",
    "// 30 Hz tick; a 16 ms budget",
    "// returns the first of two equal keys",
])
def test_what_and_why_comments_are_clean(comment):
    assert j.hits_in_comment(comment) == []


def test_strings_are_not_comments_and_waiver_skips():
    src = ('const char* s = "// B2.4 in a string";\n'
           'int a = 0; // B2.4 tag\n'
           'int b = 0; // B2.4 tag journal-comment: allow\n'
           '/* a block\n   from lane 1 */\n')
    hits = j.scan_text(src)
    assert {(ln, name) for ln, name, _ in hits} == {(2, "batch-tag"), (5, "agent-name")}


def _git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True,
                          **console_child.child_kwargs()).stdout


@pytest.fixture
def repo(tmp_path):
    r = str(tmp_path)
    _git(r, "init", "-q")
    _git(r, "config", "user.email", "t@example.invalid")
    _git(r, "config", "user.name", "t")
    (tmp_path / "a.cpp").write_text("// legacy: slice A of the old arc\nint a;\n", encoding="utf-8")
    _git(r, "add", "a.cpp")
    _git(r, "commit", "-qm", "base")
    return tmp_path


def test_diff_flags_only_added_lines(repo):
    r = str(repo)
    base = _git(r, "rev-parse", "HEAD").strip()
    (repo / "a.cpp").write_text("// legacy: slice A of the old arc\nint a;\n// WHY: b is the count\nint b;\n",
                                encoding="utf-8")
    _git(r, "commit", "-qam", "clean")
    assert j.check_diff(base, False, root=r) == []
    (repo / "a.cpp").write_text("// legacy: slice A of the old arc\nint a;\n// WHY: b is the count\nint b;\n"
                                "int c; // added in B3.4 per lane 1\n", encoding="utf-8")
    _git(r, "commit", "-qam", "journal")
    bad = j.check_diff(base, False, root=r)
    assert {(f, ln) for f, ln, _, _ in bad} == {("a.cpp", 5)}


def test_scan_tree_counts_committed_tree(repo):
    hits = j.scan_tree("HEAD", root=str(repo))
    assert [(f, ln, name) for f, ln, name, _ in hits] == [("a.cpp", 1, "step-slice-phase")]

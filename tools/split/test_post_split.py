"""What: tests for post_split.py - the plan row carries the real counts, lands after the lane's previous row, a rerun replaces it, and the board digest names failing rows.
Why: the row is copied into a plan doc other agents read; a wrong count or a misplaced row would be trusted."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import post_split as ps


def test_count_lines_matches_the_ratchet_rule():
    assert ps.count_lines(b"") == 0 and ps.count_lines(b"a\nb\n") == 2 and ps.count_lines(b"a\nb") == 2


def test_plan_row_has_counts_pieces_and_header():
    row = ps.plan_row("code/x.cpp", "x", 1316, 511, [("_progress", 381), ("_query", 415)], "stats move", "1000-1500")
    assert row.startswith("| `code/x.cpp` | 1,316 | 511 | progress, query + `x_internal.h` |")
    assert "`x_progress.cpp` (381)" in row and "`x_query.cpp` (415)" in row and "stats move" in row


def test_row_is_inserted_after_the_last_row_of_its_tier_and_keeps_crlf():
    text = "| File |\r\n| `a.cpp` | 1 | 1 | x | done (1000-1500 tier, verbatim) |\r\n| `b.cpp` | 1 | 1 | x | done (1000-1500 tier, verbatim) |\r\n\r\nafter\r\n"
    out = ps.insert_row(text, "| `c.cpp` |", "1000-1500")
    assert out.split("\r\n")[3] == "| `c.cpp` |" and out.endswith("after\r\n") and "\n" not in out.replace("\r\n", "")


def test_rerun_replaces_the_row_for_the_same_file():
    text = "| `a.cpp` | 1 | 1 | x | done (1000-1500 tier, verbatim) |\n| `b.cpp` | 1 | 1 | x | done (1000-1500 tier, verbatim) |\n"
    once = ps.insert_row(text, "| `b.cpp` | 2 | 2 | y | done (1000-1500 tier, verbatim) |", "1000-1500")
    assert once.count("`b.cpp`") == 1 and "| 2 | 2 |" in once and "`a.cpp`" in once


def test_digest_names_failing_rows_and_marks_known_ones():
    board = "...\n  WHY THE FAILING ROW(S) FAILED\n  -----\n  --- doc_links ---\n    scanned 3 md\n    re-run just this row:  (x)\n\n  --- background_launch ---\n    [bg] FAIL\n      tools/a.py:1: bad\n"
    d = ps.digest(board, {"doc_links"})
    assert [n for n, _, _ in d] == ["doc_links", "background_launch"]
    assert d[0][2] and not d[1][2] and d[1][1][0] == "[bg] FAIL"


def test_continue_flag_is_accepted_and_documented():
    import io, contextlib
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        try:
            ps.main(["--help"])
        except SystemExit:
            pass
    assert "--continue" in out.getvalue()


class _R:
    def __init__(self, rc, out=b""):
        self.returncode, self.stdout, self.stderr = rc, out, b""


def _stub(monkeypatch, tmp_path, ratchet_rc):
    plan = tmp_path / ".claude" / "cleanup"
    plan.mkdir(parents=True)
    (plan / "giant-file-split-plan.md").write_text("| `a.cpp` | 1 | 1 | x | done (1000-1500 tier, verbatim) |\n", encoding="utf-8")
    monkeypatch.setattr(ps, "REPO", str(tmp_path))
    monkeypatch.setattr(ps, "before_lines", lambda src: 100)
    monkeypatch.setattr(ps, "pieces_of", lambda src, stem: (60, [("_b", 40)], True))
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)
        if "file_size_ratchet.py" in " ".join(map(str, cmd)):
            return _R(ratchet_rc, b"file_size_ratchet: FAIL")
        if "check-contracts.sh" in " ".join(map(str, cmd)):
            return _R(0, b"  164 passed, 1 failed, 0 known-correct-failure\n")
        return _R(0, b"ok")

    monkeypatch.setattr(ps, "_run", fake_run)
    return calls


def test_ratchet_failure_stops_before_the_board_without_continue(monkeypatch, tmp_path):
    calls = _stub(monkeypatch, tmp_path, 1)
    assert ps.main(["--src", "x.cpp", "--stem", "x"]) == 1
    assert not any("check-contracts.sh" in " ".join(map(str, c)) for c in calls)


def test_continue_reports_the_ratchet_failure_and_still_runs_the_board(monkeypatch, tmp_path):
    calls = _stub(monkeypatch, tmp_path, 1)
    assert ps.main(["--src", "x.cpp", "--stem", "x", "--continue"]) == 1
    assert any("check-contracts.sh" in " ".join(map(str, c)) for c in calls)

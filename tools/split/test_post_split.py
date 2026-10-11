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

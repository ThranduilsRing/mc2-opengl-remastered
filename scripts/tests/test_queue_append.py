"""tools/queue_append.py: real-clock stamp, atomic append, newline collapse, --check (tmp queue only)."""
from __future__ import annotations

import datetime as dt
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import queue_append as qa  # noqa: E402

CLOCK = lambda: dt.datetime(2026, 11, 10, 9, 5)  # noqa: E731


def test_append_stamps_and_collapses_newlines(tmp_path):
    q = tmp_path / "q.txt"
    q.write_text("[2026-11-10 08:00] old\n", encoding="utf-8")
    assert qa.main(["a\nb\r\n\nc  ", "--queue", str(q)], clock=CLOCK) == 0
    assert q.read_text(encoding="utf-8").splitlines() == ["[2026-11-10 08:00] old", "[2026-11-10 09:05] a | b | c"]


def test_keep_newlines_file_and_stdin(tmp_path):
    q = tmp_path / "q.txt"
    f = tmp_path / "in.txt"
    f.write_text("x\ny\n", encoding="utf-8")
    assert qa.main(["--file", str(f), "--keep-newlines", "--queue", str(q)], clock=CLOCK) == 0
    assert qa.main(["--queue", str(q)], clock=CLOCK, stdin=io.StringIO("café")) == 0
    assert q.read_text(encoding="utf-8") == "[2026-11-10 09:05] x\ny\n[2026-11-10 09:05] café\n"


def test_env_queue_and_empty_refused(tmp_path, monkeypatch):
    q = tmp_path / "env.txt"
    monkeypatch.setenv("MC2_QUEUE_FILE", str(q))
    assert qa.main(["hi"], clock=CLOCK) == 0
    assert q.exists()
    assert qa.main(["   "], clock=CLOCK) == 2


def test_check_flags_out_of_order_and_future(tmp_path, capsys):
    q = tmp_path / "q.txt"
    q.write_text(
        "[2026-11-10 08:00] ok\n[2026-11-10 08:30] too late vs next\n[2026-11-10 08:10] ok\n"
        "continuation line\n[2026-11-10 23:59] future\n", encoding="utf-8")
    before = q.read_text(encoding="utf-8")
    bad = qa.check_queue(q, CLOCK)
    assert len(bad) == 2 and "line 2" in bad[0] and "later than next" in bad[0] and "line 5" in bad[1] and "later than now" in bad[1]
    assert qa.main(["--check", "--queue", str(q)], clock=CLOCK) == 1
    assert q.read_text(encoding="utf-8") == before


def test_check_clean(tmp_path):
    q = tmp_path / "q.txt"
    q.write_text("[2026-11-10 08:00] a\n[2026-11-10 08:00] b\n", encoding="utf-8")
    assert qa.check_queue(q, CLOCK) == []

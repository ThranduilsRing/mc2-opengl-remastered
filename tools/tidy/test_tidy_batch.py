"""tidy_batch: parsing, the commit message, the fx comparison and the stop conditions (no build, no clang-tidy)."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tidy_batch as tb  # noqa: E402


def test_parse_summary_takes_the_last_line():
    text = "noise\nrun_tidy_safe: 3 proven identical, 1 reverted, 0 skipped\nrun_tidy_safe: 42 proven identical, 2 reverted, 7 skipped\n"
    assert tb.parse_summary(text) == (42, 2, 7)
    assert tb.parse_summary("nothing here") is None


def test_parse_totals():
    assert tb.parse_totals("dir LG01 LG02\nTOTAL 937 691 2397\n") == (937, 691)
    assert tb.parse_totals("") is None


def test_commit_message_names_the_transform_and_counts():
    msg = tb.commit_message(18, (28, 4, 18), (693, 691), "null-tokens")
    assert msg.startswith("tidy: batch 18 - 28 TUs (4 reverted, 18 skipped)") and "--rewrite null-tokens" in msg and "LG01 693" in msg
    assert "clang-tidy safe set" in tb.commit_message(1, (1, 0, 0), None, None)


def test_same_files(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    for d in (a, b):
        d.mkdir()
        (d / "dump_full.json").write_bytes(b"x")
        (d / "rebuilt.fx").write_bytes(b"y")
    assert tb.same_files(str(a), str(b))
    (b / "rebuilt.fx").write_bytes(b"z")
    assert not tb.same_files(str(a), str(b))
    assert not tb.same_files(str(a), str(tmp_path / "missing"))


def _args(**kw):
    base = dict(dir=["mclib"], count=5, tree="build-alltargets", rewrite=None, done=None, done_set=set(), dry_run=False)
    base.update(kw)
    return argparse.Namespace(**base)


def test_an_empty_pick_stops_cleanly(monkeypatch):
    monkeypatch.setattr(tb.pick_batch, "pick", lambda *a, **k: [])
    assert tb.one_batch(_args(), 1)[0] == "empty"


def test_a_tool_that_does_not_finish_fails_the_batch(monkeypatch):
    monkeypatch.setattr(tb.pick_batch, "pick", lambda *a, **k: ["mclib/a.cpp"])
    monkeypatch.setattr(tb, "fx_roundtrip", lambda *a, **k: True)
    runner = lambda argv: subprocess.CompletedProcess(argv, 2, stdout="boom\n")      # noqa: E731
    status, detail = tb.one_batch(_args(), 1, runner)
    assert status == "failed" and "did not finish" in detail


def test_a_failed_verification_leaves_the_worktree_alone(monkeypatch):
    monkeypatch.setattr(tb.pick_batch, "pick", lambda *a, **k: ["mclib/a.cpp"])
    monkeypatch.setattr(tb, "fx_roundtrip", lambda *a, **k: True)
    monkeypatch.setattr(tb, "verify", lambda *a, **k: (False, "root build failed"))
    committed = []
    monkeypatch.setattr(tb._git_util, "commit_paths", lambda *a, **k: committed.append(a))
    runner = lambda argv: subprocess.CompletedProcess(argv, 0, stdout="run_tidy_safe: 1 proven identical, 0 reverted, 0 skipped\n")   # noqa: E731
    status, detail = tb.one_batch(_args(), 1, runner)
    assert status == "failed" and "root build failed" in detail and not committed


def test_the_ledger_records_what_was_picked(monkeypatch, tmp_path):
    monkeypatch.setattr(tb.pick_batch, "pick", lambda *a, **k: ["mclib/a.cpp", "mclib/b.cpp"])
    ledger = tmp_path / "done.txt"
    args = _args(done=str(ledger), dry_run=True)
    tb.one_batch(args, 1)
    assert ledger.read_text().split() == ["mclib/a.cpp", "mclib/b.cpp"] and args.done_set == {"mclib/a.cpp", "mclib/b.cpp"}

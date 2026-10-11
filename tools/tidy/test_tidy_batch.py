"""tidy_batch: parsing, the commit message, the fx comparison and the stop conditions (no build, no clang-tidy)."""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pytest  # noqa: E402
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
    base = dict(dir=["mclib"], count=5, tree="build-alltargets", rewrite=None, done=None, done_set=set(), dry_run=False,
                proof="auto", corpus=False, log_identity=False)
    base.update(kw)
    return argparse.Namespace(**base)


@pytest.fixture(autouse=True)
def clean_worktree(monkeypatch):
    monkeypatch.setattr(tb._git_util, "dirty_paths", lambda *a, **k: set())


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
    seen = []
    monkeypatch.setattr(tb._git_util, "dirty_paths", lambda *a, **k: seen.append(1) or (set() if len(seen) == 1 else {"mclib/a.cpp"}))
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


def test_a_dirty_worktree_is_refused_before_anything_runs(monkeypatch):
    monkeypatch.setattr(tb.pick_batch, "pick", lambda *a, **k: ["mclib/a.cpp"])
    monkeypatch.setattr(tb._git_util, "dirty_paths", lambda *a, **k: {"code/x.cpp"})
    ran = []
    status, detail = tb.one_batch(_args(), 1, lambda argv: ran.append(argv))
    assert status == "failed" and "uncommitted" in detail and not ran


def _compile_flow(monkeypatch, log_compare, dirty_after=("code/a.cpp", "tests/unit/printf_corpus.jsonl")):
    """Scripted fakes for the whole printf pipeline; -> (calls, args)."""
    calls = []
    monkeypatch.setattr(tb.pick_batch, "pick", lambda *a, **k: ["code/a.cpp"])
    monkeypatch.setattr(tb, "fx_roundtrip", lambda *a, **k: True)
    state = {"n": 0}

    def dirty(*a, **k):
        state["n"] += 1
        return set() if state["n"] == 1 else set(dirty_after)         # clean at the start, edited after run_tidy_safe

    monkeypatch.setattr(tb._git_util, "dirty_paths", dirty)
    monkeypatch.setattr(tb.printf_corpus, "add", lambda files, base: calls.append(("corpus.add", list(files), base)) or 1)
    monkeypatch.setattr(tb.printf_corpus, "write_generated", lambda: calls.append(("corpus.write",)) or 1)
    monkeypatch.setattr(tb, "verify", lambda *a, **k: calls.append(("verify", k.get("census"), k.get("everything"))) or (True, "ok"))
    monkeypatch.setattr(tb._git_util, "commit_paths", lambda root, paths, msg, trailer=None: calls.append(("commit", list(paths), msg)) or "abc123")
    monkeypatch.setattr(tb, "ensure_deployed", lambda runner=None: calls.append(("deploy",)) or None)
    monkeypatch.setattr(tb, "capture_logs", lambda dest, runner=None: calls.append(("logs", os.path.basename(dest))) or None)
    monkeypatch.setattr(tb.log_identity, "compare", lambda a, b: log_compare)

    def runner(argv):
        text = "run_tidy_safe: 1 proven compiling, 0 reverted, 0 skipped\n" if "run_tidy_safe.py" in " ".join(map(str, argv)) else "TOTAL 1 2 3\n"
        return subprocess.CompletedProcess(argv, 0, stdout=text)

    return calls, runner


def test_compile_proof_with_corpus_and_logs_runs_in_order_and_commits(monkeypatch):
    calls, runner = _compile_flow(monkeypatch, ([], [], True))
    status, detail = tb.one_batch(_args(rewrite="printf-buffers", corpus=True, log_identity=True), 7, runner)
    assert status == "committed", detail
    kinds = [c[0] for c in calls]
    assert kinds[:3] == ["deploy", "logs", "corpus.add"] and kinds.count("deploy") == 2 and kinds.count("logs") == 2
    assert ("corpus.add", ["code/a.cpp"], "HEAD") in calls
    assert ("verify", True, True) in calls                    # census on, whole build (standalone harnesses) on
    commit = next(c for c in calls if c[0] == "commit")
    assert "tests/unit/printf_corpus.jsonl" in commit[1] and "format corpus" in commit[2] and "batch 7" in commit[2]


def test_differing_logs_fail_the_batch_after_the_commit(monkeypatch):
    calls, runner = _compile_flow(monkeypatch, (["m.log: name=abc   n=#"], [], True))
    status, detail = tb.one_batch(_args(rewrite="printf-buffers", corpus=True, log_identity=True), 8, runner)
    assert status == "failed" and "abc123" in detail and "game logs differ" in detail


def test_objhash_proof_stays_the_default_for_other_rewriters(monkeypatch):
    calls, runner = _compile_flow(monkeypatch, ([], [], True))
    status, _ = tb.one_batch(_args(rewrite="null-tokens"), 9, runner)
    assert status == "committed" and ("verify", False, False) in calls and not any(c[0] in ("deploy", "corpus.add") for c in calls)

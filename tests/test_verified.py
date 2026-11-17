from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

from scripts import console_child
from tools import verified

SHA = "abcdef1" + "0" * 33
OTHER = "1" * 40


def resolve(revision):
    if revision in {SHA, "abcdef1", "HEAD"}:
        return SHA
    if revision == OTHER:
        return OTHER
    raise ValueError("unknown or ambiguous commit")


def test_short_and_full_commit_merge_and_query(tmp_path):
    ledger = tmp_path / "verified.json"
    first = verified.record_proof(ledger, "abcdef1", {"build": "build-17", "captures": ["cap-a"]}, resolver=resolve)
    updated = verified.record_proof(ledger, SHA, {"tier1": "tier1-42", "captures": ["cap-a", "cap-b"]}, resolver=resolve)
    assert first["board"] is None
    assert updated["build"] == "build-17"
    assert updated["captures"] == ["cap-a", "cap-b"]
    assert verified.show_proof(ledger, "HEAD", resolver=resolve) == updated
    assert verified.latest_proof(ledger) == updated
    assert verified.read_ledger(ledger)["records"] == [updated]
    verified.record_proof(ledger, OTHER, {"board": "b"}, resolver=resolve)
    assert verified.latest_proof(ledger)["sha"] == OTHER


def test_git_resolution_and_console_spawn(monkeypatch):
    calls = []
    monkeypatch.setattr(verified.console_child, "child_kwargs", lambda **kw: {"sentinel": kw["redirected"]})
    def run(args, **kwargs):
        calls.append((args, kwargs))
        return SimpleNamespace(returncode=0, stdout=SHA + "\n")
    monkeypatch.setattr(verified.subprocess, "run", run)
    assert verified.resolve_commit("abcdef1") == SHA
    assert calls[0][0][-2:] == ["--end-of-options", "abcdef1^{commit}"]
    assert calls[0][1]["sentinel"] is True
    monkeypatch.setattr(verified.subprocess, "run", lambda *a, **k: SimpleNamespace(returncode=128, stdout=""))
    for revision in ("missing", "ambiguous"):
        with pytest.raises(ValueError, match="unknown or ambiguous"):
            verified.resolve_commit(revision)


def test_unknown_commits_never_create_ledger(tmp_path):
    path = tmp_path / "ledger.json"
    for revision in ("missing", "ambiguous"):
        with pytest.raises(ValueError, match="unknown or ambiguous"):
            verified.record_proof(path, revision, {"build": "b"}, resolver=resolve)
    assert not path.exists()


@pytest.mark.parametrize("mutation", [
    lambda row: row.update(captures=None), lambda row: row.update(captures=[1]),
    lambda row: row.update(captures=["x", "x"]), lambda row: row.update(build=[]),
    lambda row: row.update(tier1=42), lambda row: row.update(sha="abc1234"),
    lambda row: row.update(sha="z" * 40), lambda row: row.pop("doctest"),
    lambda row: row.update(extra="bad"),
])
def test_malformed_schema(mutation):
    row = {"sha": SHA, **{f: None for f in verified.FIELDS}, "captures": []}
    mutation(row)
    with pytest.raises(ValueError):
        verified.validate_ledger({"schema_version": 1, "records": [row]})


def test_malformed_schema_is_rejected_before_mutation(tmp_path):
    path = tmp_path / "ledger.json"
    row = {"sha": SHA, **{f: None for f in verified.FIELDS}, "captures": None}
    text = json.dumps({"schema_version": 1, "records": [row]})
    path.write_text(text, encoding="utf-8")
    with pytest.raises(ValueError):
        verified.record_proof(path, SHA, {"board": "new"}, resolver=resolve)
    assert path.read_text(encoding="utf-8") == text


def test_duplicate_shas_and_bad_updates(tmp_path):
    path = tmp_path / "ledger.json"
    row = verified.record_proof(path, SHA, {"build": "b"}, resolver=resolve)
    path.write_text(json.dumps({"schema_version": 1, "records": [row, row]}), encoding="utf-8")
    with pytest.raises(ValueError, match="unique canonical"):
        verified.read_ledger(path)
    for updates in ({}, {"build": []}, {"captures": "bad"}, {"unknown": "x"}):
        with pytest.raises(ValueError):
            verified.record_proof(path, SHA, updates, resolver=resolve)


def test_threaded_writers_preserve_every_capture(tmp_path):
    path = tmp_path / "ledger.json"
    def write(i):
        return verified.record_proof(path, SHA, {"captures": [f"cap-{i}"]}, resolver=resolve)
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(write, range(24)))
    assert set(verified.latest_proof(path)["captures"]) == {f"cap-{i}" for i in range(24)}
    assert path.with_name(path.name + ".lock").read_bytes() == b""


def test_process_writers_preserve_every_capture(tmp_path):
    path = tmp_path / "ledger.json"
    children = [subprocess.Popen([sys.executable, "-B", str(verified.ROOT / "tools/verified.py"),
                                 "record", "HEAD", "--ledger", str(path), "--capture", f"process-{i}"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                **console_child.child_kwargs(redirected=True)) for i in range(6)]
    for child in children:
        out, err = child.communicate(timeout=45)
        assert child.returncode == 0, (out, err)
    assert set(verified.latest_proof(path)["captures"]) == {f"process-{i}" for i in range(6)}


def test_interrupted_replace_preserves_previous_snapshot_and_releases_lock(tmp_path, monkeypatch):
    path = tmp_path / "ledger.json"
    verified.record_proof(path, SHA, {"build": "old"}, resolver=resolve)
    original = path.read_bytes()
    with monkeypatch.context() as patch:
        def interrupted(*args):
            raise OSError("interrupted replace")
        patch.setattr(verified.os, "replace", interrupted)
        with pytest.raises(OSError, match="interrupted"):
            verified.record_proof(path, SHA, {"build": "new"}, resolver=resolve)
    assert path.read_bytes() == original
    assert not list(tmp_path.glob("*.tmp"))
    verified.record_proof(path, SHA, {"board": "after"}, resolver=resolve)


def test_cli_schema_errors_and_empty_check(tmp_path, capsys):
    path = tmp_path / "ledger.json"
    path.write_text("{bad", encoding="utf-8")
    assert verified.main(["latest", "--ledger", str(path)]) == 2
    path.write_text(json.dumps(verified.empty_ledger()), encoding="utf-8")
    assert verified.main(["check", "--ledger", str(path)]) == 0
    assert verified.main(["latest", "--ledger", str(path)]) == 1

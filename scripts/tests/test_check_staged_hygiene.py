"""Real Git blobs/attributes prove INDEX and RANGE scans independently of working files.

Scratch repositories are retained for review; this suite never recursively
deletes. T1C_TEST_SCRATCH may select a writable parent directory.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

import pytest


SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import console_child

SPEC = importlib.util.spec_from_file_location("check_staged_hygiene", SCRIPTS / "check-staged-hygiene.py")
hygiene = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hygiene)


def git(repo, *args, data=None):
    env = os.environ.copy()
    for name in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
        env.pop(name, None)
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return subprocess.run(
        ["git", *args], cwd=repo, input=data, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=True, timeout=15, env=env,
        **console_child.child_kwargs(redirected=True),
    ).stdout


@pytest.fixture(scope="session")
def scratch():
    parent = os.environ.get("T1C_TEST_SCRATCH", tempfile.gettempdir())
    root = Path(parent) / f"mc2-t1c-{uuid.uuid4().hex}"
    root.mkdir()
    # An inaccessible scratch parent is blocked proof; mkdtemp retries that
    # Windows permission failure for minutes instead of exposing the cause.
    (root / "write-probe.txt").write_text("T1c scratch access\n", encoding="ascii")
    print(f"T1c retained scratch: {root}")
    return root


@pytest.fixture
def repo(scratch, monkeypatch):
    for name in ("GIT_INDEX_FILE", "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR"):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", os.devnull)
    root = scratch / f"repo-{uuid.uuid4().hex}"
    root.mkdir()
    git(root, "init", "--quiet")
    git(root, "config", "user.name", "Hygiene test")
    git(root, "config", "user.email", "hygiene@example.invalid")
    git(root, "config", "core.autocrlf", "false")
    git(root, "config", "core.safecrlf", "false")
    git(root, "config", "commit.gpgsign", "false")
    git(root, "config", "core.hooksPath", str(root / "no-hooks"))
    git(root, "commit", "--quiet", "--allow-empty", "-m", "baseline")
    return root


def stage(repo, path, data):
    target = repo / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    git(repo, "add", "--", path)


def baseline(repo, path, data):
    stage(repo, path, data)
    git(repo, "commit", "--quiet", "-m", "fixture")


def scan(repo, capsys, expected):
    assert hygiene.main(["--repo", str(repo)]) == expected
    return capsys.readouterr().out


@pytest.mark.parametrize("before,after,expected", [
    (b"old\r\n", b"new\r\n", False),
    (b"old\r\n", b"new\n", True),
    (b"old\n", b"new\r\n", True),
    (b"old\n", b"new\nmore\r\n", True),
    (None, b"new\nmore\r\n", True),
    (None, b"new\r\n", False),
    (b"old", b"old\na\r\n", True),
    (b"old", b"old\na\n", False),
    (b"old", b"old\r\na\r\n", False),
    (b"old\x08\n", b"new\x08\n", False),
    (b"old\x08\n", b"new\x07\n", True),
    (b"old\n", b"new\0\n", True),
    (b"old\n", b"new\t\n", False),
    (b"old\n", b"new", False),
])
def test_blob_contract_without_scratch(before, after, expected):
    assert bool(hygiene.findings(before, after)) == expected


def test_batch_reads_all_blobs_with_one_process(monkeypatch):
    contents = {f"{i:040x}": b"body\n\0\r\n" + bytes([i]) for i in range(1, 101)}
    ids = list(contents)
    files = [(f"file-{i}.txt", ids[i], ids[i + 1]) for i in range(99)]
    files.append(("added.txt", "0" * 40, ids[0]))
    calls = []

    def batch_run(command, **kwargs):
        calls.append(command)
        assert command == ["git", "cat-file", "--batch"]
        assert kwargs["input"] == "".join(f"{blob}\n" for blob in ids).encode("ascii")
        output = b"".join(
            f"{blob} blob {len(body)}\n".encode("ascii") + body + b"\n"
            for blob, body in contents.items()
        )
        return subprocess.CompletedProcess(command, 0, stdout=output, stderr=b"")

    monkeypatch.setattr(hygiene.subprocess, "run", batch_run)
    assert hygiene.read_blobs(Path.cwd(), files) == contents
    assert len(calls) == 1


@pytest.mark.parametrize("response", [
    b"missing missing\n", b"", b"{blob} tree 0\n\n", b"{blob} blob 3\nab\n",
    b"{blob} blob -1\n\n", b"{blob} blob 0\n\nextra",
])
def test_batch_protocol_errors_fail_closed(monkeypatch, response):
    blob = "1" * 40
    monkeypatch.setattr(hygiene, "git", lambda *args, **kwargs: response.replace(b"{blob}", blob.encode()))
    with pytest.raises(ValueError):
        hygiene.read_blobs(Path.cwd(), [("file.txt", "0" * 40, blob)])


def test_no_staged_changes_ignores_dirty_worktree(repo, capsys):
    baseline(repo, "file.txt", b"old\n")
    (repo / "file.txt").write_bytes(b"bad\x08\r\n")
    assert "0 text checked" in scan(repo, capsys, 0)


@pytest.mark.parametrize("old,new", [(b"a\r\nb\r\n", b"a\nb\n"), (b"a\nb\n", b"a\r\nb\r\n")])
def test_uniform_eol_drift(repo, capsys, old, new):
    baseline(repo, "file.txt", old)
    stage(repo, "file.txt", new)
    assert "EOL drift" in scan(repo, capsys, 1)


@pytest.mark.parametrize("ending", [b"\n", b"\r\n"])
def test_preserves_committed_style(repo, capsys, ending):
    baseline(repo, "file.txt", b"old" + ending)
    stage(repo, "file.txt", b"new" + ending)
    scan(repo, capsys, 0)


@pytest.mark.parametrize("ending", [b"\n", b"\r\n"])
def test_uniform_to_mixed_fails(repo, capsys, ending):
    baseline(repo, "file.txt", b"a" + ending + b"b" + ending)
    stage(repo, "file.txt", b"a\nb\r\n")
    assert "INDEX mixed" in scan(repo, capsys, 1)


def test_newline_free_head_cannot_gain_mixed_endings(repo, capsys):
    baseline(repo, "file.txt", b"old")
    stage(repo, "file.txt", b"old\na\r\n")
    assert "mixed LF/CRLF" in scan(repo, capsys, 1)


def test_existing_mixed_endings_are_grandfathered(repo, capsys):
    baseline(repo, "file.txt", b"old\na\r\n")
    stage(repo, "file.txt", b"new\na\r\n")
    scan(repo, capsys, 0)


@pytest.mark.parametrize("content,result", [(b"a\n", 0), (b"a\r\n", 0), (b"a", 0), (b"a\nb\r\n", 1), (b"a\x08\n", 1)])
def test_new_files(repo, capsys, content, result):
    stage(repo, "new.txt", content)
    scan(repo, capsys, result)


@pytest.mark.parametrize("value", [*range(0x00, 0x09), 0x0B, 0x0C, *range(0x0E, 0x20), 0x7F])
def test_each_forbidden_control_in_existing_text(repo, capsys, value):
    baseline(repo, "file.txt", b"old\n")
    stage(repo, "file.txt", b"old" + bytes([value]) + b"\n")
    assert f"0x{value:02X} at byte 3" in scan(repo, capsys, 1)


def test_existing_control_only_allowed_at_same_offset_and_value(repo, capsys):
    baseline(repo, "file.txt", b"a\x08b\n")
    stage(repo, "file.txt", b"z\x08b\n")
    scan(repo, capsys, 0)
    stage(repo, "file.txt", b"a\x07b\n")
    assert "0x07 at byte 1" in scan(repo, capsys, 1)
    stage(repo, "file.txt", b"aa\x08b\n")
    assert "0x08 at byte 2" in scan(repo, capsys, 1)
    stage(repo, "file.txt", b"ab\n")
    scan(repo, capsys, 0)


@pytest.mark.parametrize("staged_bad", [True, False])
def test_partial_staging_reads_index_not_worktree(repo, capsys, staged_bad):
    baseline(repo, "file.txt", b"old\r\n")
    clean, bad = b"new\r\n", b"new\x08\n"
    stage(repo, "file.txt", bad if staged_bad else clean)
    (repo / "file.txt").write_bytes(clean if staged_bad else bad)
    scan(repo, capsys, int(staged_bad))


@pytest.mark.parametrize("attribute", [b"binary", b"-text"])
def test_binary_attributes_skip_even_without_nul(repo, capsys, attribute):
    baseline(repo, ".gitattributes", b"*.bin " + attribute + b"\n")
    baseline(repo, "file.bin", b"old\n")
    stage(repo, "file.bin", b"new\x08\r\n")
    assert "1 skipped" in scan(repo, capsys, 0)


@pytest.mark.parametrize("new_file", [True, False])
def test_nul_binary_heuristic(repo, capsys, new_file):
    if not new_file:
        baseline(repo, "file.bin", b"old\0\n")
    stage(repo, "file.bin", b"new\0\x08\n\r\n")
    assert "1 skipped" in scan(repo, capsys, 0)


def test_auto_text_keeps_binary_heuristic(repo, capsys):
    baseline(repo, ".gitattributes", b"*.bin text=auto\n")
    baseline(repo, "file.bin", b"old\0\n")
    stage(repo, "file.bin", b"new\0\x08\r\n")
    assert "1 skipped" in scan(repo, capsys, 0)


def test_explicit_text_checks_nul_in_new_file(repo, capsys):
    baseline(repo, ".gitattributes", b"*.txt text\n")
    stage(repo, "file.txt", b"a\0b\x08\n")
    output = scan(repo, capsys, 1)
    assert "0x00" in output and "0x08" in output


@pytest.mark.parametrize("eol", ["lf", "crlf"])
def test_eol_policy_checks_normalized_blobs(repo, capsys, eol):
    baseline(repo, ".gitattributes", f"*.txt text eol={eol}\n".encode())
    baseline(repo, "file.txt", b"old\r\n")
    stage(repo, "file.txt", b"new\r\n")
    assert git(repo, "cat-file", "blob", ":file.txt") == b"new\n"
    scan(repo, capsys, 0)


def test_eol_attribute_implies_text(repo, capsys):
    baseline(repo, ".gitattributes", b"*.txt eol=lf\n")
    stage(repo, "file.txt", b"a\0\x08\n")
    assert "0x00" in scan(repo, capsys, 1)


def test_staged_attributes_ignore_unstaged_override(repo, capsys):
    baseline(repo, ".gitattributes", b"*.txt text\n")
    stage(repo, "file.txt", b"bad\0\x08\n")
    (repo / ".gitattributes").write_bytes(b"*.txt -text\n")
    scan(repo, capsys, 1)
    stage(repo, ".gitattributes", b"*.txt -text\n")
    (repo / ".gitattributes").write_bytes(b"*.txt text\n")
    assert "1 skipped" in scan(repo, capsys, 0)


@pytest.mark.parametrize("encoding,bom", [("utf-16-le", b"\xff\xfe"), ("utf-16-be", b"\xfe\xff"), ("utf-32-le", b"\xff\xfe\0\0"), ("utf-32-be", b"\0\0\xfe\xff")])
def test_wide_bom_files_skipped_even_with_text_attribute(repo, capsys, encoding, bom):
    baseline(repo, ".gitattributes", b"*.txt text\n")
    baseline(repo, "file.txt", bom + "old\r\n".encode(encoding))
    stage(repo, "file.txt", bom + "new\x08\n".encode(encoding))
    assert "1 skipped" in scan(repo, capsys, 0)


@pytest.mark.parametrize("forced_text", [False, True])
@pytest.mark.parametrize("encoding,bom", [
    ("utf-16-le", b"\xff\xfe"), ("utf-16-be", b"\xfe\xff"),
    ("utf-32-le", b"\xff\xfe\0\0"), ("utf-32-be", b"\0\0\xfe\xff"),
])
def test_wide_head_does_not_exempt_utf8_index(repo, capsys, forced_text, encoding, bom):
    if forced_text:
        baseline(repo, ".gitattributes", b"*.txt text\n")
    baseline(repo, "file.txt", bom + "old\r\n".encode(encoding))
    stage(repo, "file.txt", b"new\x08\n")
    assert "0x08 at byte 3" in scan(repo, capsys, 1)


def test_committed_bad_file_fails_explicit_range_with_clean_index(repo, capsys):
    baseline(repo, "file.txt", b"old\n")
    baseline(repo, "file.txt", b"bad\x08\n")
    assert "0 text checked" in scan(repo, capsys, 0)
    assert hygiene.main(["--repo", str(repo), "--range", "HEAD~1..HEAD"]) == 1
    output = capsys.readouterr().out
    assert "0x08 at byte 3" in output and "RANGE" in output


def test_default_task_range_covers_all_commits_since_merge_base(repo, capsys):
    baseline(repo, "file.txt", b"old\n")
    git(repo, "branch", "claude/nifty-mendeleev")
    git(repo, "checkout", "--quiet", "-b", "claude/tooling-t1")
    base = git(repo, "rev-parse", "HEAD").strip().decode()
    baseline(repo, "file.txt", b"bad\x08\n")
    baseline(repo, "good.txt", b"good\n")
    assert hygiene.main(["--repo", str(repo), "--range", "HEAD~1..HEAD"]) == 0
    capsys.readouterr()
    assert hygiene.main(["--repo", str(repo), "--range"]) == 1
    output = capsys.readouterr().out
    assert f"RANGE {base}.." in output and "0x08" in output


def test_default_main_branch_range_checks_last_commit(repo, capsys):
    baseline(repo, "old.txt", b"bad\x08\n")
    git(repo, "checkout", "--quiet", "-b", "claude/nifty-mendeleev")
    base = git(repo, "rev-parse", "HEAD").strip().decode()
    baseline(repo, "new.txt", b"bad\x07\n")
    assert hygiene.main(["--repo", str(repo), "--range"]) == 1
    output = capsys.readouterr().out
    assert f"RANGE {base}.." in output and "new.txt" in output and "old.txt" not in output


@pytest.mark.parametrize("binary", [False, True])
def test_range_uses_tip_attributes_and_blobs(repo, capsys, binary):
    baseline(repo, ".gitattributes", b"*.txt text\n")
    baseline(repo, "file.txt", b"old\n")
    base = git(repo, "rev-parse", "HEAD").strip().decode()
    stage(repo, ".gitattributes", b"*.txt -text\n" if binary else b"*.txt text\n")
    stage(repo, "file.txt", b"bad\x08\n")
    git(repo, "commit", "--quiet", "-m", "destination")
    stage(repo, ".gitattributes", b"*.txt text\n" if binary else b"*.txt -text\n")
    stage(repo, "file.txt", b"clean\n")
    (repo / ".gitattributes").write_bytes(b"*.txt -text\n")
    assert hygiene.main(["--repo", str(repo), "--range", f"{base}..HEAD"]) == int(not binary)
    output = capsys.readouterr().out
    assert "1 skipped" in output if binary else "0x08" in output


@pytest.mark.parametrize("value", ["HEAD", "HEAD...HEAD", "HEAD..", "..HEAD", "absent..HEAD"])
def test_invalid_range_returns_input_error(repo, capsys, value):
    assert hygiene.main(["--repo", str(repo), "--range", value]) == 2
    assert "staged-hygiene:" in capsys.readouterr().err


def test_missing_default_base_fails_instead_of_scanning_index(repo, capsys):
    assert hygiene.main(["--repo", str(repo), "--range"]) == 2
    assert "merge-base" in capsys.readouterr().err


def test_deletion_is_ignored(repo, capsys):
    baseline(repo, "file.txt", b"old\x08\r\n")
    (repo / "file.txt").unlink()
    git(repo, "add", "-u", "--", "file.txt")
    assert "0 text checked" in scan(repo, capsys, 0)


@pytest.mark.parametrize("bad", [True, False])
def test_rename_uses_original_head_blob(repo, capsys, bad):
    original = b"alpha\r\nbeta\r\ngamma\r\ndelta\r\nepsilon\r\n"
    baseline(repo, "old name.txt", original)
    git(repo, "mv", "--", "old name.txt", "new name.txt")
    stage(repo, "new name.txt", original.replace(b"\r\n", b"\n") if bad else original)
    assert b"R" in git(repo, "diff", "--cached", "--name-status", "--find-renames")
    scan(repo, capsys, int(bad))


def test_unborn_head_is_supported(repo, capsys):
    git(repo, "checkout", "--orphan", "unborn", "--quiet")
    stage(repo, "file.txt", b"bad\x08\n")
    assert "new control" in scan(repo, capsys, 1)


def test_nonregular_index_objects_are_skipped(repo, capsys):
    blob = git(repo, "hash-object", "-w", "--stdin", data=b"bad\x08\n").strip().decode()
    git(repo, "update-index", "--add", "--cacheinfo", f"120000,{blob},link")
    head = git(repo, "rev-parse", "HEAD").strip().decode()
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{head},module")
    assert "0 text checked" in scan(repo, capsys, 0)


def test_cli_quiet_and_subdirectory(repo, capsys):
    subdir = repo / "subdir"
    subdir.mkdir()
    assert hygiene.main(["--repo", str(subdir), "--quiet"]) == 0
    assert capsys.readouterr().out == ""
    stage(repo, "file.txt", b"bad\x08\n")
    result = subprocess.run(
        [sys.executable, "-B", str(SCRIPTS / "check-staged-hygiene.py"), "--quiet"],
        cwd=subdir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15,
        **console_child.child_kwargs(redirected=True),
    )
    assert result.returncode == 1
    assert b"FAIL 'file.txt'" in result.stdout


def test_git_error_returns_two(scratch, capsys):
    assert hygiene.main(["--repo", str(scratch)]) == 2
    assert "staged-hygiene:" in capsys.readouterr().err


def test_staged_alias_scans_index(repo, capsys):
    # --staged is the pre-commit spelling of --index: a heredoc NUL and a CRLF flip both fail it.
    baseline(repo, "a.txt", b"one\n")
    baseline(repo, "b.txt", b"xy\n")
    stage(repo, "a.txt", b"one\r\n")
    stage(repo, "b.txt", b"x\x00y\n")
    assert hygiene.main(["--repo", str(repo), "--staged"]) == 1
    out = capsys.readouterr().out
    assert "EOL drift" in out and "0x00" in out

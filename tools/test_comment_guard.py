"""Tests for tools/comment_guard.py (throwaway git repos and synthetic COFF objects; stdlib + git only).

Run:
  py -3 -B -m pytest tools/test_comment_guard.py -q -p no:cacheprovider
"""
import importlib.util
import json
import os
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

_spec = importlib.util.spec_from_file_location("comment_guard", os.path.join(HERE, "comment_guard.py"))
cg = importlib.util.module_from_spec(_spec)
sys.modules["comment_guard"] = cg
_spec.loader.exec_module(cg)

TOOL = os.path.join(HERE, "comment_guard.py")


def _run(*args, cwd=None):
    r = subprocess.run([sys.executable, "-B", TOOL] + list(args), cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout.decode("utf-8", "replace")


def _git(repo, *args):
    subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t"] + list(args),
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                   **console_child.child_kwargs(redirected=True))


@pytest.fixture
def repo(tmp_path):
    _git(tmp_path, "init", "-q")
    return tmp_path


def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8") if isinstance(text, str) else text)


# ---------------------------------------------------------------- selftest fixtures
@pytest.mark.parametrize("fn", cg.SELFTEST_CASES, ids=lambda f: f.__name__)
def test_selftest_case(fn):
    fn()


def test_selftest_cli_exit_zero():
    rc, out = _run("--selftest")
    assert rc == 0, out
    assert "ok" in out


def test_selftest_has_the_required_coverage():
    names = " ".join(f.__name__ for f in cg.SELFTEST_CASES)
    for needle in ("string_literal", "protected_deletion", "adding_comment", "multiline_block", "raw_string",
                   "gates", "blank_line", "crlf", "bom", "moved_comment"):
        assert needle in names, needle


# ---------------------------------------------------------------- CLI via --base-file
def test_cli_base_file_ok_and_report_line(tmp_path):
    base, new = tmp_path / "a.cpp", tmp_path / "b.cpp"
    _write(base, "int g;\n// narration one\nint f() { return 1; } // trailing\n")
    _write(new, "int g;\nint f() { return 1; }\n")
    rc, out = _run("--check", "--base-file", str(base), str(new), "--repo", str(tmp_path))
    assert rc == 0, out
    assert "OK" in out and "comments" in out and "%" in out


def test_cli_base_file_code_change_exit1_with_line_pair(tmp_path):
    base, new = tmp_path / "a.cpp", tmp_path / "b.cpp"
    _write(base, "int g;\nint f() { return 1; }\n")
    _write(new, "int g;\nint f() { return 2; }\n")
    rc, out = _run("--base-file", str(base), str(new), "--repo", str(tmp_path))
    assert rc == 1
    assert "return 1" in out and "return 2" in out


def test_cli_base_file_needs_exactly_one_path_exit2(tmp_path):
    base = tmp_path / "a.cpp"
    _write(base, "int g;\n")
    rc, _ = _run("--base-file", str(base), "--repo", str(tmp_path))
    assert rc == 2


def test_cli_unreadable_base_file_exit2(tmp_path):
    new = tmp_path / "b.cpp"
    _write(new, "int g;\n")
    rc, _ = _run("--base-file", str(tmp_path / "missing.cpp"), str(new), "--repo", str(tmp_path))
    assert rc == 2


def test_cli_bad_option_exit2():
    rc, _ = _run("--no-such-option")
    assert rc == 2


# ---------------------------------------------------------------- git sources
def test_git_worktree_vs_head(repo):
    f = repo / "code" / "a.cpp"
    _write(f, "int g;\n// narration\nint a;\n")
    _git(repo, "add", "code/a.cpp")
    _git(repo, "commit", "-q", "-m", "base")
    _write(f, "int g;\nint a;\n")
    rc, out = _run("--repo", str(repo), "--check", str(f))
    assert rc == 0, out
    _write(f, "int g;\nint b;\n")
    rc, out = _run("--repo", str(repo), "--check", str(f))
    assert rc == 1 and "code differs" in out


def test_git_staged_and_default_file_list(repo):
    f = repo / "code" / "a.cpp"
    _write(f, "int g;\n// narration\nint a;\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    _write(f, "int g;\nint a;\n")
    _git(repo, "add", "code/a.cpp")
    _write(f, "int g;\nint tampered;\n")          # worktree differs from the index: --staged must ignore it
    rc, out = _run("--repo", str(repo), "--check", "--staged")
    assert rc == 0, out
    rc, out = _run("--repo", str(repo), "--check")
    assert rc == 1, out


def test_git_new_untracked_file_has_no_base(repo):
    keep = repo / "keep.txt"
    _write(keep, "x\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    f = repo / "code" / "new.cpp"
    _write(f, "int a;\n")
    rc, out = _run("--repo", str(repo), "--check", str(f))
    assert rc == 1 and "no-base" in out


def test_git_out_of_scope_file_fails_unless_ignored(repo):
    f = repo / "notes.md"
    _write(f, "# a\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    _write(f, "# b\n")
    assert _run("--repo", str(repo), "--check", str(f))[0] == 1
    assert _run("--repo", str(repo), "--check", "--ignore-out-of-scope", str(f))[0] == 0


def test_git_gates_file_any_change_rejected(repo):
    f = repo / "Gates" / "gate_table.cpp"
    _write(f, "int a;\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    _write(f, "int a; \n")
    rc, out = _run("--repo", str(repo), "--check", str(f))
    assert rc == 1 and "Gates" in out


def test_path_outside_repo_exit2(repo, tmp_path_factory):
    other = tmp_path_factory.mktemp("elsewhere") / "a.cpp"
    _write(other, "int a;\n")
    assert _run("--repo", str(repo), "--check", str(other))[0] == 2


def test_base_rev_option(repo):
    f = repo / "a.cpp"
    _write(f, "int g;\n// one\nint a;\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "c1")
    _write(f, "int g;\nint a;\n")
    _git(repo, "commit", "-q", "-am", "c2")
    rc, out = _run("--repo", str(repo), "--check", "--base", "HEAD~1", str(f))
    assert rc == 0, out
    rc, out = _run("--repo", str(repo), "--check", "--rev", "HEAD", str(f))
    assert rc == 0, out


# ---------------------------------------------------------------- unit-level behaviour
def test_protected_reasons_are_reported():
    rep = cg.check_texts("a.cpp", b"int a;\n// the race is deliberate\nint b;\n", b"int a;\nint b;\n")
    assert [k for k, _ in rep.findings] == ["protected"]
    assert "race" in rep.findings[0][1] and "deliberate" in rep.findings[0][1]


def test_comment_bytes_accounting():
    rep = cg.check_texts("a.cpp", b"int a; // abcd\n/* efgh */\n", b"int a; // ab\n")
    assert rep.ok
    assert rep.base_cb == len("// abcd") + len("/* efgh */")
    assert rep.new_cb == len("// ab")


def test_lexer_comment_markers_inside_literals_do_not_split():
    lx = cg.lex('const char* s = "a // b /* c */";\n')
    assert lx.comments == []
    assert len(lx.logical) == 1


def test_lexer_line_numbers_survive_multiline_comments():
    lx = cg.lex("/* a\nb\nc */ int x;\nint y;\n")
    assert [ln for ln, _t in lx.logical] == [3, 4]


# ---------------------------------------------------------------- object proof
def make_obj(ts=1, text=b"\x90\x90\xc3", dbg=b"line-table-1", rdata=b"hello"):
    """Smallest COFF object the parser understands: .text, .rdata, .debug$S, one symbol."""
    secs = [(".text$mn", text), (".rdata", rdata), (".debug$S", dbg)]
    hdr, shdr = 20, 40 * len(secs)
    off, entries, blob = hdr + shdr, [], b""
    for name, data in secs:
        entries.append((name, off + len(blob), len(data)))
        blob += data
    symptr = hdr + shdr + len(blob)
    out = struct.pack("<HHIIIHH", 0x8664, len(secs), ts, symptr, 1, 0, 0)
    for name, ptr, size in entries:
        out += struct.pack("<8sIIIIIIHHI", name.encode(), 0, 0, size, ptr, 0, 0, 0, 0, 0x60000020)
    out += blob
    out += struct.pack("<8sIhHBB", b"main", 0, 1, 0x20, 2, 0)
    out += struct.pack("<I", 4)
    return out


def test_obj_digest_ignores_timestamp_and_debug_sections(tmp_path):
    a, b, c = tmp_path / "a.obj", tmp_path / "b.obj", tmp_path / "c.obj"
    a.write_bytes(make_obj(ts=1, dbg=b"AAAA"))
    b.write_bytes(make_obj(ts=999, dbg=b"a much longer debug payload"))
    c.write_bytes(make_obj(text=b"\x90\xc3"))
    da, db, dc = (cg.obj_digest(str(p)) for p in (a, b, c))
    assert da["format"] == "coff"
    assert da["code_sha256"] == db["code_sha256"]
    assert da["raw_sha256"] != db["raw_sha256"]
    assert da["code_sha256"] != dc["code_sha256"]


def _stamp(path, when):
    os.utime(path, ns=(when, when))


def _obj_fixture(tmp_path, src_text="int a;\n"):
    repo = tmp_path / "repo"
    build = tmp_path / "build"
    src = repo / "code" / "a.cpp"
    _write(src, src_text)
    obj = build / "CMakeFiles" / "mc2.dir" / "code" / "a.cpp.obj"
    obj.parent.mkdir(parents=True)
    obj.write_bytes(make_obj())
    base = 2_000_000_000 * 10**9
    _stamp(src, base)
    _stamp(obj, base + 10**9)
    return repo, build, src, obj, base


def test_hash_then_compare_objs_roundtrip(tmp_path, capsys):
    repo, build, src, obj, base = _obj_fixture(tmp_path)
    assert cg.main(["--repo", str(repo), "--hash-objs", str(build), str(src)]) == 0
    before = tmp_path / "before.json"
    before.write_text(capsys.readouterr().out, encoding="utf-8")
    rec = json.loads(before.read_text(encoding="utf-8"))
    assert rec["files"]["code/a.cpp"]["status"] == "ok"

    # not rebuilt -> refused, even though the bytes are identical
    assert cg.main(["--compare-objs", str(before)]) == 1
    assert "not rebuilt" in capsys.readouterr().out

    # rebuilt with only a timestamp and debug-table change -> identical behaviour
    obj.write_bytes(make_obj(ts=77, dbg=b"shifted lines"))
    _stamp(src, base + 2 * 10**9)
    _stamp(obj, base + 3 * 10**9)
    assert cg.main(["--compare-objs", str(before)]) == 0
    assert "OK" in capsys.readouterr().out

    # rebuilt with changed code -> mismatch
    obj.write_bytes(make_obj(text=b"\xcc"))
    _stamp(obj, base + 4 * 10**9)
    assert cg.main(["--compare-objs", str(before)]) == 1
    assert "FAIL" in capsys.readouterr().out


def test_line_sensitive_source_downgrades_mismatch_to_warn(tmp_path, capsys):
    repo, build, src, obj, base = _obj_fixture(tmp_path, "int l = __LINE__;\n")
    assert cg.main(["--repo", str(repo), "--hash-objs", str(build), str(src)]) == 0
    before = tmp_path / "before.json"
    before.write_text(capsys.readouterr().out, encoding="utf-8")
    assert json.loads(before.read_text(encoding="utf-8"))["files"]["code/a.cpp"]["warn"]
    obj.write_bytes(make_obj(rdata=b"world"))
    _stamp(obj, base + 5 * 10**9)
    assert cg.main(["--compare-objs", str(before)]) == 0
    assert "WARN" in capsys.readouterr().out


def test_hash_objs_refuses_stale_or_missing_objects(tmp_path, capsys):
    repo, build, src, obj, base = _obj_fixture(tmp_path)
    _stamp(src, base + 9 * 10**9)                     # source edited after the object was built
    assert cg.main(["--repo", str(repo), "--hash-objs", str(build), str(src)]) == 2
    capsys.readouterr()
    obj.unlink()
    assert cg.main(["--repo", str(repo), "--hash-objs", str(build), str(src)]) == 2


def test_hash_objs_skips_headers(tmp_path, capsys):
    repo, build, src, obj, base = _obj_fixture(tmp_path)
    hdr = repo / "code" / "a.h"
    _write(hdr, "int a;\n")
    assert cg.main(["--repo", str(repo), "--hash-objs", str(build), str(hdr)]) == 0
    assert json.loads(capsys.readouterr().out)["files"]["code/a.h"]["status"] == "skip"

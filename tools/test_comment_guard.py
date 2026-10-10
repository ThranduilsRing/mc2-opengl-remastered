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
                   "gates", "blank_line", "crlf", "bom", "moved_comment", "checker_needle",
                   "begin_end", "fenced", "split_piece", "needle_extraction"):
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
def make_obj(ts=1, text=b"\x90\x90\xc3", dbg=b"line-table-1", rdata=b"hello", chks=None):
    """Smallest COFF object the parser understands: .text, .rdata, .debug$S, one symbol (+ .chks64 when given)."""
    secs = [(".text$mn", text), (".rdata", rdata), (".debug$S", dbg)]
    if chks is not None:
        secs.append((".chks64", chks))
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


# ---------------------------------------------------------------- loosened gates, rewrite tag, allowlist
GATED = "int a;\n// MC2_REAL_GATE=1 turns this on\nint b;\n"
HELD = "int a;\n// the lock must be held\nint b;\n"
TAG = cg.REWRITE_TAG


@pytest.fixture
def gates():
    cg.set_gate_names({"MC2_REAL_GATE"})
    yield
    cg.set_gate_names(None)


def _enc(s):
    return s.encode("utf-8")


def test_registered_gate_protects_unregistered_macro_does_not(gates):
    assert not cg.check_texts("code/x.cpp", _enc(GATED), _enc("int a;\nint b;\n")).ok
    macro = "int a;\n// MC2_VERIFY (slice MISSION-DATA-DEREF-HARDEN-1) added\nint b;\n"
    assert cg.check_texts("code/x.cpp", _enc(macro), _enc("int a;\nint b;\n")).ok


def test_real_gate_table_is_the_default_source():
    cg.set_gate_names(None)
    names = cg.registered_gates()
    assert names and "MC2_VERIFY" not in names
    some = sorted(names)[0]
    rep = cg.check_texts("code/x.cpp", _enc("int a;\n// %s on\nint b;\n" % some), _enc("int a;\nint b;\n"))
    assert [k for k, _m in rep.findings] == ["protected"]


def test_unreadable_gate_table_protects_every_mc2_token():
    cg.set_gate_names(False)
    try:
        assert not cg.check_texts("code/x.cpp", _enc("// MC2_VERIFY\nint a;\n"), _enc("int a;\n")).ok
    finally:
        cg.set_gate_names(None)


def test_slice_tag_alone_not_protective_but_keywords_beside_it_are(gates):
    for tag in ("BUILDING-GLASS-1", "SHADOW-FRUSTUM-AUDIT-1"):
        assert cg.check_texts("code/x.cpp", _enc("int a;\n// %s\nint b;\n" % tag), _enc("int a;\nint b;\n")).ok
    rep = cg.check_texts("code/x.cpp", _enc("int a;\n// FOO-BAR-1 never reorder\nint b;\n"), _enc("int a;\nint b;\n"))
    assert not rep.ok


def test_never_must_do_not_rules_unchanged(gates):
    for s in ("Guard 2: packet_oob - globalPktIdx must be valid index", "never null", "do not reorder"):
        rep = cg.check_texts("code/x.cpp", _enc("int a;\n// %s\nint b;\n" % s), _enc("int a;\nint b;\n"))
        assert [k for k, _m in rep.findings] == ["protected"], s


def test_tagged_comment_survives_and_altered_body_fails(gates):
    ok = "int a;\n// %s the lock must be held\nint b;\n" % TAG
    assert cg.check_texts("code/x.cpp", _enc(HELD), _enc(ok)).ok
    bad = "int a;\n// %s the lock should be held\nint b;\n" % TAG
    assert not cg.check_texts("code/x.cpp", _enc(HELD), _enc(bad)).ok
    block = "int a;\n/* %s the lock must be held */\nint b;\n" % TAG
    assert cg.check_texts("code/x.cpp", _enc("int a;\n/* the lock must be held */\nint b;\n"), _enc(block)).ok


def test_tag_growth_budget_is_16_bytes_per_new_tag(gates):
    tagged = "int a;\n// %s the lock must be held\nint b;\n" % TAG
    assert cg.check_texts("code/x.cpp", _enc(HELD), _enc(tagged)).ok                      # +15 B, one tag
    padded = tagged.replace("held", "held right here now")
    rep = cg.check_texts("code/x.cpp", _enc(HELD), _enc(padded))
    assert not rep.ok                                                                      # body changed anyway
    two = "int a;\n// %s x\n// %s y\nint b;\n" % (TAG, TAG)
    assert cg.check_texts("code/x.cpp", _enc("int a;\n// x\n// y\nint b;\n"), _enc(two)).ok  # 2 tags -> +32 B ok
    fat = "int a;\n// %s x\nint b; // extra commentary words\n" % TAG
    assert "grew" in [k for k, _m in cg.check_texts("code/x.cpp", _enc("int a;\n// x\nint b;\n"), _enc(fat)).findings]


def test_cli_rewrite_candidates_lists_file_line_text(repo):
    f = repo / "code" / "a.cpp"
    _write(f, "int g;\n// %s odd legacy note\nint a;\n/* %s second\n   line */\n" % (TAG, TAG))
    rc, out = _run("--repo", str(repo), "--rewrite-candidates", str(f))
    assert rc == 0, out
    assert "code/a.cpp:2: odd legacy note" in out and "code/a.cpp:4: second line" in out


def test_cli_rewrite_candidates_scans_tracked_files_without_paths(repo):
    _write(repo / "code" / "a.cpp", "int g;\n// %s keep me\n" % TAG)
    _write(repo / "code" / "b.cpp", "int g;\n// plain\n")
    _git(repo, "add", "-A")
    rc, out = _run("--repo", str(repo), "--rewrite-candidates")
    assert rc == 0 and "code/a.cpp:2: keep me" in out and "b.cpp" not in out


def _allow_file(tmp_path, entries):
    p = tmp_path / "allow.json"
    p.write_text(json.dumps(entries), encoding="utf-8")
    return str(p)


def _allow_pair(tmp_path):
    base, new = tmp_path / "base.cpp", tmp_path / "new.cpp"
    _write(base, HELD)
    _write(new, "int a;\nint b;\n")
    return base, new


def test_cli_allowlist_permits_listed_deletion_and_prints_trail(tmp_path):
    base, new = _allow_pair(tmp_path)
    al = _allow_file(tmp_path, [{"file": "new.cpp", "base_line": 3, "reason": "restates the guard"}])   # +-2
    rc, out = _run("--repo", str(tmp_path), "--base-file", str(base), "--allow", al, str(new))
    assert rc == 0, out
    assert "ALLOWED-DELETE new.cpp:2 restates the guard" in out


def test_cli_allowlist_wrong_line_fails_and_is_stale(tmp_path):
    base, new = _allow_pair(tmp_path)
    al = _allow_file(tmp_path, [{"file": "new.cpp", "base_line": 40, "reason": "r"}])
    rc, out = _run("--repo", str(tmp_path), "--base-file", str(base), "--allow", al, str(new))
    assert rc == 2 and "protected comment" in out and "ALLOWED-DELETE" not in out


def test_cli_allowlist_stale_entry_is_an_error_even_when_the_file_passes(tmp_path):
    base, new = tmp_path / "base.cpp", tmp_path / "new.cpp"
    _write(base, "int a;\n// plain narration\nint b;\n")
    _write(new, "int a;\nint b;\n")
    al = _allow_file(tmp_path, [{"file": "new.cpp", "base_line": 2, "reason": "nothing protected here"}])
    rc, _out = _run("--repo", str(tmp_path), "--base-file", str(base), "--allow", al, str(new))
    assert rc == 2


def test_cli_allowlist_malformed_is_usage_error(tmp_path):
    base, new = _allow_pair(tmp_path)
    for bad in ([{"file": "new.cpp", "base_line": 2}], {"file": "new.cpp"}, [{"file": "new.cpp", "base_line": "2", "reason": "r"}]):
        assert _run("--repo", str(tmp_path), "--base-file", str(base), "--allow", _allow_file(tmp_path, bad), str(new))[0] == 2


def test_allowlist_entry_covers_one_comment_only(gates):
    two = "int a;\n// the lock must be held\n\n// it must never be null\nint b;\n"
    al = cg.Allowlist([{"file": "code/x.cpp", "base_line": 3, "reason": "r"}])
    rep = cg.check_texts("code/x.cpp", _enc(two), _enc("int a;\nint b;\n"), al)
    assert [k for k, _m in rep.findings] == ["protected"] and len(rep.allowed) == 1


# ---------------------------------------------------------------- checker needles, begin/end markers, split pieces
@pytest.fixture(autouse=True)
def _hermetic_env():
    """Unit tests never read the real repo's needles; the tests below that want some build their own tree."""
    cg.set_env(cg.Env({}, []))
    yield
    cg.set_env(None)


SENTINEL_CPP = "void f() {\n\tscan();\n\t// NOFOCUS-HARDEN-1\n}\n"
SENTINEL_GONE = "void f() {\n\tscan();\n}\n"


def _checker_repo(repo, checker_text, rel="scripts/check-sentinel.sh"):
    _write(repo / rel, checker_text)
    _write(repo / "code" / "a.cpp", SENTINEL_CPP)
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    _write(repo / "code" / "a.cpp", SENTINEL_GONE)


def test_cli_deleted_sentinel_is_reported_with_needle_and_script(repo):
    _checker_repo(repo, "#!/bin/sh\ngrep -q 'NOFOCUS-HARDEN-1' code/a.cpp\n")
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
    assert rc == 1, out
    assert "checker-needle" in out and "NOFOCUS-HARDEN-1" in out and "scripts/check-sentinel.sh" in out, out


def test_cli_same_tag_absent_from_checkers_is_not_protected(repo):
    _checker_repo(repo, "#!/bin/sh\ngrep -q 'SOME-OTHER-TAG-9' code/a.cpp\n")
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
    assert rc == 0, out


def test_cli_needle_in_a_plain_python_string_counts(repo):
    _checker_repo(repo, "TAG = \"NOFOCUS-HARDEN-1\"\n\n\ndef check(text):\n    return TAG in text\n", rel="scripts/check_s.py")
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
    assert rc == 1 and "scripts/check_s.py" in out, out


def test_needle_in_a_checker_comment_or_docstring_does_not_count(repo):
    _checker_repo(repo, '"""Mentions NOFOCUS-HARDEN-1 in prose."""\n# NOFOCUS-HARDEN-1 again\n', rel="scripts/check_s.py")
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
    assert rc == 0, out


def test_cli_needles_lists_needle_and_first_defining_file(repo):
    _write(repo / "scripts" / "a_check.sh", "grep -n 'FIRST-SENTINEL-END' x\n")
    _write(repo / "scripts" / "b_check.sh", "grep -n 'FIRST-SENTINEL-END' x\ngrep -c 'SECOND_MARKER:' x\n")
    _write(repo / "tools" / "test_skipped.py", "X = 'TEST-ONLY-TAG-1'\n")
    _write(repo / "tools" / "comment_guard.py", "X = 'SELF-ONLY-TAG-1'\n")
    _write(repo / "tests" / "t.py", "X = 'TESTS-DIR-TAG-1'\n")
    _write(repo / "cmake" / "c.cmake", "if(x MATCHES \"CMAKE-SENTINEL-1\")\nendif()\n")
    rc, out = _run("--repo", str(repo), "--needles")
    lines = out.strip().splitlines()
    assert rc == 0
    assert "FIRST-SENTINEL-END\tscripts/a_check.sh" in lines and "SECOND_MARKER:\tscripts/b_check.sh" in lines, out
    assert "TESTS-DIR-TAG-1\ttests/t.py" in lines and "CMAKE-SENTINEL-1\tcmake/c.cmake" in lines, out
    assert "TEST-ONLY-TAG-1" not in out and "SELF-ONLY-TAG-1" not in out


def test_cli_needles_on_this_repo_lists_the_mover_sentinel():
    rc, out = _run("--needles")
    assert rc == 0
    assert "MOVER-COHERENCE-AUDIT-END\tscripts/check-mover-registry-coherence.sh" in out.splitlines(), out[:400]


def test_begin_end_and_fenced_markers_are_protected_without_any_checker():
    for line in ("S6-REGION-BEGIN", "S6-REGION-END (scan region)", "@@ keep me @@", "=== section ==="):
        rep = cg.check_texts("code/x.cpp", _enc("int a;\n// %s\nint b;\n" % line), _enc("int a;\nint b;\n"))
        assert [k for k, _m in rep.findings] == ["protected"], line


def _split_repo(repo, with_allow=True):
    if with_allow:
        _write(repo / "tools" / "split" / "allow" / "foo.json",
               json.dumps({"src": "code/foo.cpp", "stem": "foo", "base": "abc1234"}))
    _write(repo / "code" / "foo_bar.cpp", '#include "foo_internal.h"\n// plain description of this piece\nint a;\n')
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    _write(repo / "code" / "foo_bar.cpp", '#include "foo_internal.h"\nint a;\n')


def test_split_piece_description_is_protected_when_the_stem_has_an_allow_entry(repo):
    _split_repo(repo)
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "foo_bar.cpp"))
    assert rc == 1 and "split-piece header" in out, out


def test_split_piece_description_not_protected_without_an_allow_entry(repo):
    _split_repo(repo, with_allow=False)
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "foo_bar.cpp"))
    assert rc == 0, out


def test_cli_needles_keep_markers_and_drop_prose_and_identifiers(repo):
    _write(repo / "scripts" / "c.sh", "grep -q 'byte-identical' x\ngrep -q gos_terrain x\ngrep -q MC2_VK x\n"
           "grep -q 'TIER2-EXCLUDED' x\ngrep -q 'SAMPLE_MARKER:' x\ngrep -q 'Some-long-Marker-word' x\n")
    rc, out = _run("--repo", str(repo), "--needles")
    got = [l.split("\t")[0] for l in out.strip().splitlines()]
    assert rc == 0 and sorted(got) == ["SAMPLE_MARKER:", "Some-long-Marker-word", "TIER2-EXCLUDED"], got


def test_marker_rule_and_stoplist_unit():
    assert cg.looks_like_marker("TEX-CLASS:") and cg.looks_like_marker("TIER2-EXCLUDED")
    assert not any(cg.looks_like_marker(w) for w in cg.NEEDLE_STOPLIST)
    assert not cg.looks_like_marker("any_lowercase_stem") and not cg.looks_like_marker("short-Id")


# ---------------------------------------------------------------- line endings (autocrlf artifacts)
def _eol_pair(tmp_path, base, new):
    b, n = tmp_path / "b.cpp", tmp_path / "n.cpp"
    b.write_bytes(base)
    n.write_bytes(new)
    return _run("--repo", str(tmp_path), "--base-file", str(b), str(n))


def test_uniform_crlf_vs_lf_passes_with_a_note(tmp_path):
    rc, out = _eol_pair(tmp_path, b"int z;\r\n// narration\r\nint a;\r\n", b"int z;\nint a;\n")
    assert rc == 0 and "note: line endings normalised (CRLF" in out, out
    rc, out = _eol_pair(tmp_path, b"int z;\n// narration\nint a;\n", b"int z;\r\nint a;\r\n")
    assert rc == 0 and "line endings normalised" in out, out


def test_uniform_eol_change_still_fails_on_a_code_change(tmp_path):
    rc, out = _eol_pair(tmp_path, b"int z;\r\nint a;\r\n", b"int z;\nint b;\n")
    assert rc == 1 and "code differs" in out, out


def test_mixed_line_endings_fail(tmp_path):
    rc, out = _eol_pair(tmp_path, b"int z;\r\n// n\r\nint a;\r\n", b"int z;\r\nint a;\n")
    assert rc == 1 and "mixed" in out, out


def test_bom_change_fails_even_with_uniform_endings(tmp_path):
    rc, out = _eol_pair(tmp_path, b"\xef\xbb\xbfint z;\r\nint a;\r\n", b"int z;\nint a;\n")
    assert rc == 1 and "BOM removed" in out, out


# ---------------------------------------------------------------- external needles file, MFC ClassWizard markers
PROJECTZ = "re:\\[PROJECTZ:[A-Za-z]+ id=[^\\]]+\\]  # external oracle tool reads these ids\n"


def _oracle_repo(repo, needles_text):
    _write(repo / "tools" / "comment_guard_needles.txt", needles_text)
    _write(repo / "code" / "a.cpp", "void f() {\n\t// [PROJECTZ:ScreenXYOracle id=a_screen]\n\tg();\n}\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "base")
    _write(repo / "code" / "a.cpp", "void f() {\n\tg();\n}\n")


def test_cli_external_regex_needle_protects_oracle_id(repo):
    _oracle_repo(repo, PROJECTZ)
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
    assert rc == 1 and "PROJECTZ:ScreenXYOracle id=a_screen" in out and "comment_guard_needles.txt (external)" in out, out


def test_cli_without_a_needles_file_the_id_comment_is_plain(repo):
    _oracle_repo(repo, "# no needles\n")
    rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
    assert rc == 0, out


def test_cli_needles_marks_external_entries(repo):
    _write(repo / "tools" / "comment_guard_needles.txt", PROJECTZ + "OUTSIDE-READER-9  # literal one\n")
    rc, out = _run("--repo", str(repo), "--needles")
    lines = out.strip().splitlines()
    assert rc == 0
    assert "OUTSIDE-READER-9\ttools/comment_guard_needles.txt (external)" in lines, out
    assert any(l.startswith("re:") and l.endswith("(external)") for l in lines), out


def test_cli_malformed_needles_line_is_a_usage_error(repo):
    _oracle_repo(repo, PROJECTZ)
    for bad in ("OUTSIDE-READER-9\n", "re:(unclosed  # reason\n", "re:a*  # matches the empty string\n"):
        _write(repo / "tools" / "comment_guard_needles.txt", PROJECTZ + bad)
        rc, out = _run("--repo", str(repo), "--check", str(repo / "code" / "a.cpp"))
        assert rc == 2, (bad, out)
        assert _run("--repo", str(repo), "--needles")[0] == 2


def test_real_needles_file_is_well_formed_and_seeded():
    with open(os.path.join(HERE, "comment_guard_needles.txt"), encoding="utf-8") as fh:
        lits, res = cg.parse_needle_file(fh.read())
    assert any("PROJECTZ" in src for _rx, src, _l in res)
    assert any(rx.search("[PROJECTZ:SelectionPicking id=x_1]") for rx, _s, _l in res)


def test_mfc_classwizard_markers_survive_the_cli(tmp_path):
    base, new = tmp_path / "b.cpp", tmp_path / "n.cpp"
    _write(base, "class C {\n\t//{{AFX_MSG(C)\n\tvoid f();\n\t//}}AFX_MSG\n};\n")
    _write(new, "class C {\n\tvoid f();\n};\n")
    rc, out = _run("--repo", str(tmp_path), "--base-file", str(base), str(new))
    assert rc == 1 and "ClassWizard" in out, out


# ---------------------------------------------------------------- --allow entries after a restore
def test_cli_allow_entry_for_comment_present_in_base_and_new_is_unused_info_exit0(tmp_path):
    base, new = tmp_path / "base.cpp", tmp_path / "new.cpp"
    _write(base, HELD)
    _write(new, HELD)
    al = _allow_file(tmp_path, [{"file": "new.cpp", "base_line": 2, "reason": "deleted before the restore"}])
    rc, out = _run("--repo", str(tmp_path), "--base-file", str(base), "--allow", al, str(new))
    assert rc == 0, out
    assert "UNUSED-ALLOW new.cpp:2 deleted before the restore" in out, out


def test_cli_allow_entry_for_comment_absent_in_base_is_stale_exit2(tmp_path):
    base, new = tmp_path / "base.cpp", tmp_path / "new.cpp"
    _write(base, "int a;\n// plain narration\nint b;\n")
    _write(new, "int a;\nint b;\n")
    al = _allow_file(tmp_path, [{"file": "new.cpp", "base_line": 2, "reason": "nothing to forgive"}])
    r = subprocess.run([sys.executable, "-B", TOOL, "--repo", str(tmp_path), "--base-file", str(base), "--allow", al,
                        str(new)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 2
    assert "STALE-ALLOW new.cpp:2" in r.stderr.decode("utf-8", "replace") and b"UNUSED-ALLOW" not in r.stdout

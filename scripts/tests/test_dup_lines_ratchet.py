"""Tests for tools/dup_lines_ratchet.py (synthetic fixtures; stdlib + git only).

Run:
  py -3 -B -m pytest scripts/tests/test_dup_lines_ratchet.py -q -p no:cacheprovider
"""
import importlib.util
import io
import json
import os
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("dup_lines_ratchet", os.path.join(ROOT, "tools", "dup_lines_ratchet.py"))
dlr = importlib.util.module_from_spec(_spec)
sys.modules["dup_lines_ratchet"] = dlr
_spec.loader.exec_module(dlr)


def block(tag, n=10):
    """n distinct non-trivial statements; the tag keeps different blocks from matching each other."""
    return "".join("    int %s_value_%d = compute_%s(%d);\n" % (tag, i, tag, i) for i in range(n))


_uid = [0]


def cpp(*parts):
    """A function body around parts; the head and tail lines are unique so only parts can match."""
    _uid[0] += 1
    return "void f_%d() {\n" % _uid[0] + "".join(parts) + "    int tail_%d = 0;\n}\n" % _uid[0]


def test_cross_file_duplicate_is_detected():
    shared = block("a", 10)
    res = dlr.scan({"mclib/x.cpp": cpp(shared), "code/y.cpp": cpp("    int pad = 1;\n", shared)}, n=8)
    assert res.per_file["mclib/x.cpp"] == 10 and res.per_file["code/y.cpp"] == 10
    assert res.total == 20
    assert res.per_dir == {"mclib": 10, "code": 10}
    assert len(res.groups) == 1 and res.groups[0][1] == 10        # one maximal 10-line run, not 3 windows
    assert {s[0] for s in res.groups[0][2]} == {"mclib/x.cpp", "code/y.cpp"}


def test_intra_file_duplicate_is_detected():
    res = dlr.scan({"GameOS/z.cpp": cpp(block("a"), "    int sep = 0;\n", block("a"))}, n=8)
    assert res.total == 20
    assert len(res.groups[0][2]) == 2


def test_unique_code_reports_nothing():
    res = dlr.scan({"mclib/a.cpp": cpp(block("a")), "mclib/b.cpp": cpp(block("b"))}, n=8)
    assert res.total == 0 and not res.groups


def test_run_length_threshold_respected():
    seven = block("a", 7)
    files = {"mclib/a.cpp": cpp(seven, "    int q1 = 1;\n"), "mclib/b.cpp": cpp(seven, "    int q2 = 2;\n")}
    assert dlr.scan(files, n=8).total == 0
    assert dlr.scan(files, n=7).total == 14


def test_comment_only_duplicates_are_ignored():
    com = "".join("    // this is explanatory comment line number %d\n" % i for i in range(12))
    blk = "/*\n" + "".join(" * block comment line %d\n" % i for i in range(12)) + " */\n"
    res = dlr.scan({"mclib/a.cpp": cpp(com, blk, "    int a1 = 1;\n"),
                    "mclib/b.cpp": cpp(com, blk, "    int b1 = 2;\n")}, n=8)
    assert res.total == 0


def test_comments_and_whitespace_do_not_hide_a_copy():
    a = block("a", 9)
    b = "".join("\t  " + l.strip().replace(" = ", "   =   ") + "   // note %d\n" % i
                for i, l in enumerate(a.splitlines()))
    res = dlr.scan({"mclib/a.cpp": cpp(a), "mclib/b.cpp": cpp("/* hi */\n", b)}, n=8)
    assert res.total == 18


def test_comment_text_inside_string_literal_is_kept():
    lines = "".join('    log("http://host/%d");\n' % i for i in range(9))
    res = dlr.scan({"mclib/a.cpp": cpp(lines), "mclib/b.cpp": cpp(lines)}, n=8)
    assert res.total == 18


def test_trivial_and_repetitive_windows_are_ignored():
    braces = "{\n}\n" * 20
    inc = "".join("#include <h%d.h>\n" % i for i in range(12))
    row = "    0, 0, 0, 0,\n" * 20
    files = {"mclib/a.cpp": braces + inc + row + "int a;\n", "mclib/b.cpp": braces + inc + row + "int b;\n"}
    assert dlr.scan(files, n=8).total == 0


def test_self_overlapping_periodic_windows_do_not_double_count():
    period = "".join("    step_%d();\n" % i for i in range(4))
    res = dlr.scan({"mclib/a.cpp": cpp(period * 4)}, n=8)
    # 16 lines of period 4: only windows 8 rows apart count as a copy, so each line is counted once
    assert res.total == 16


def test_unowned_dirs_exclusion_and_generated_files():
    blk = cpp(block("a"))
    pats = dlr.BUILTIN_EXCLUDES + ["shaders/legacy/", "mclib/skipme.cpp"]
    assert dlr.wanted("mclib/a.cpp", dlr.DEFAULT_DIRS, pats)
    assert not dlr.wanted("editor/a.cpp", dlr.DEFAULT_DIRS, pats)            # not an owned dir
    assert not dlr.wanted("mclib/readme.txt", dlr.DEFAULT_DIRS, pats)        # not source
    assert not dlr.wanted("RenderCore/gpu_abi/X.layout.gen.h", dlr.DEFAULT_DIRS, pats)
    assert not dlr.wanted("shaders/include/a.gen.hglsl", dlr.DEFAULT_DIRS, pats)
    assert not dlr.wanted("shaders/spv/a.frag", dlr.DEFAULT_DIRS, pats)
    assert not dlr.wanted("code/3rdparty/a.cpp", dlr.DEFAULT_DIRS, pats)
    assert not dlr.wanted("shaders/legacy/a.frag", dlr.DEFAULT_DIRS, pats)   # exclude-file dir glob
    assert not dlr.wanted("mclib/skipme.cpp", dlr.DEFAULT_DIRS, pats)        # exclude-file exact path
    assert not dlr.wanted("mclib/build64/a.cpp", dlr.DEFAULT_DIRS, pats)
    assert blk  # fixture sanity


def test_gitattributes_linguist_generated_is_honoured():
    attrs = "*.zip filter=lfs\n*.gen.h text eol=lf\nshaders/baked/*.glsl linguist-generated=true\nx/*.h linguist-generated\ny/*.h -linguist-generated\n"
    assert dlr.gitattr_generated(attrs) == ["shaders/baked/*.glsl", "x/*.h"]


def test_exclude_file_parsing():
    assert dlr.parse_patterns("# c\n\nmclib/a/\n  *.inc  \n") == ["mclib/a/", "*.inc"]


def _res(total, **per_dir):
    r = dlr.Result(8)
    r.total, r.per_dir = total, per_dir
    return r


def _base(total, **counts):
    return {"head": "abc", "n": 8, "total": total, "counts": counts, "files": {}}


def test_check_passes_when_equal_or_lower():
    out = io.StringIO()
    assert dlr.check(_res(20, mclib=12, code=8), _base(20, mclib=12, code=8), out) == 0
    assert dlr.check(_res(15, mclib=10, code=5), _base(20, mclib=12, code=8), out) == 0


def test_check_fails_when_total_rises():
    out = io.StringIO()
    assert dlr.check(_res(21, mclib=13, code=8), _base(20, mclib=12, code=8), out) == 1
    assert "TOTAL" in out.getvalue() and "mclib" in out.getvalue()


def test_check_fails_when_one_dir_rises_even_if_total_falls():
    out = io.StringIO()
    assert dlr.check(_res(18, mclib=9, code=9), _base(20, mclib=12, code=8), out) == 1
    assert "code" in out.getvalue() and "TOTAL" not in out.getvalue()


def test_scan_is_deterministic():
    files = {"mclib/a.cpp": cpp(block("a"), block("b")), "code/b.cpp": cpp(block("b"), block("a")),
             "GameOS/c.cpp": cpp(block("a"))}
    r1, r2 = dlr.scan(files, 8), dlr.scan(dict(reversed(list(files.items()))), 8)
    assert (r1.total, r1.per_dir, r1.groups) == (r2.total, r2.per_dir, r2.groups)


# ---- end-to-end against a throwaway git repo: HEAD is the source of truth, not the worktree
def _git(repo, *args):
    subprocess.run(["git", "-C", str(repo)] + list(args), check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                   env=dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                            GIT_COMMITTER_EMAIL="t@t"), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


@pytest.fixture
def repo(tmp_path):
    _git(tmp_path, "init", "-q")
    for rel, txt in {"mclib/a.cpp": cpp(block("a")), "code/b.cpp": cpp(block("a")),
                     "code/gen.gen.h": cpp(block("a")), "tools/dup_lines_ratchet.exclude": "# none\n"}.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(txt)
    _git(tmp_path, "add", ".")
    _git(tmp_path, "commit", "-qm", "init")
    return tmp_path


def test_cli_baseline_check_roundtrip_reads_head_only(repo):
    base = str(repo / "base.json")
    common = ["--repo", str(repo), "--baseline", base]
    assert dlr.main(["--write-baseline"] + common) == 0
    saved = json.load(open(base))
    assert saved["total"] == 20 and saved["counts"]["mclib"] == 10 and saved["counts"]["code"] == 10
    assert dlr.main(["--check"] + common) == 0

    # an uncommitted copy in the working tree must not flap the HEAD check
    (repo / "GameOS").mkdir()
    (repo / "GameOS" / "c.cpp").write_text(cpp(block("a")))
    assert dlr.main(["--check"] + common) == 0
    assert dlr.main(["--check", "--worktree"] + common) == 1

    # committing it raises the count
    _git(repo, "add", "GameOS/c.cpp")
    _git(repo, "commit", "-qm", "third copy")
    assert dlr.main(["--check"] + common) == 1


def test_cli_missing_baseline_is_usage_error(repo):
    assert dlr.main(["--check", "--repo", str(repo), "--baseline", str(repo / "nope.json")]) == 2


def test_cli_tighten_lowers_baseline(repo):
    base = str(repo / "base.json")
    common = ["--repo", str(repo), "--baseline", base]
    dlr.main(["--write-baseline"] + common)
    (repo / "code" / "b.cpp").write_text(cpp(block("zzz")))
    _git(repo, "add", "code/b.cpp")
    _git(repo, "commit", "-qm", "dedupe")
    assert dlr.main(["--check", "--tighten"] + common) == 0
    assert json.load(open(base))["total"] == 0

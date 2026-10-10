"""Tests for tools/file_size_ratchet.py (throwaway git repos; stdlib + git only).

Run:
  py -3 -B -m pytest scripts/tests/test_file_size_ratchet.py -q -p no:cacheprovider
"""
import importlib.util
import io
import json
import os
import subprocess
import sys

import pytest
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("file_size_ratchet", os.path.join(ROOT, "tools", "file_size_ratchet.py"))
fsr = importlib.util.module_from_spec(_spec)
sys.modules["file_size_ratchet"] = fsr
_spec.loader.exec_module(fsr)


def lines(n):
    return "".join("int v%d;\n" % i for i in range(n))


def _git(repo, *args):
    subprocess.run(["git", "-C", str(repo)] + list(args), check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                   env=dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                            GIT_COMMITTER_EMAIL="t@t"), **console_child.child_kwargs(redirected=True))


def put(repo, rel, n):
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(lines(n), newline="\n")


def commit(repo, msg, *paths):
    _git(repo, "add", *paths)
    _git(repo, "commit", "-qm", msg)


# ---- measurement
def test_count_lines_is_physical():
    assert fsr.count_lines(b"") == 0
    assert fsr.count_lines(b"a\n") == 1
    assert fsr.count_lines(b"a\n\n\n") == 3            # blank lines count
    assert fsr.count_lines(b"a\nb") == 2               # unterminated last line counts
    assert fsr.count_lines("// c\n/* c */\n") == 2     # comments count


def test_measure_per_dir_metrics():
    res = fsr.measure({"mclib/a.cpp": lines(1500), "mclib/b.cpp": lines(1501), "mclib/c.cpp": lines(3001),
                       "code/d.cpp": lines(10)})
    assert res.per_dir["mclib"] == {"files": 3, "files_over_1500": 2, "files_over_3000": 1, "max_lines": 3001}
    assert res.per_dir["code"]["files_over_1500"] == 0 and res.per_dir["code"]["max_lines"] == 10
    assert res.total["files_over_1500"] == 2 and res.total["max_lines"] == 3001


def test_wanted_dirs_extensions_and_builtin_exclusions():
    pats = fsr.BUILTIN_EXCLUDES + ["mclib/skipme.cpp", "shaders/legacy/"]
    ok = lambda r: fsr.wanted(r, fsr.DEFAULT_DIRS, pats)
    assert ok("mclib/a.cpp") and ok("shaders/x.hglsl") and ok("editor/a.h") and ok("gui/a.inl")
    assert ok("shaders/x.comp") and ok("shaders/x.tese")
    assert not ok("Viewer/a.cpp")                          # not an owned dir
    assert not ok("mclib/readme.txt")                      # not source
    assert not ok("mclib/a.c")                             # .c not in the list
    assert not ok("RenderCore/gpu_abi/X.layout.gen.h")     # generated
    assert not ok("shaders/include/a.gen.hglsl")
    assert not ok("shaders/spv/a.frag")
    assert not ok("code/3rdparty/a.cpp")
    assert not ok("mclib/build64/a.cpp")
    assert not ok("mclib/skipme.cpp") and not ok("shaders/legacy/a.frag")   # exclude-file globs


def test_gitattributes_linguist_generated_is_honoured():
    attrs = "*.gen.h text\nshaders/baked/*.glsl linguist-generated=true\nx/*.h linguist-generated\ny/*.h -linguist-generated\n"
    assert fsr.gitattr_generated(attrs) == ["shaders/baked/*.glsl", "x/*.h"]


# ---- check() logic
def _base(**kw):
    res = fsr.measure({"mclib/a.cpp": lines(kw.get("a", 2000)), "code/b.cpp": lines(kw.get("b", 100))})
    return {"head": "abc", "per_dir": res.per_dir, "total": res.total, "files_over_1500": fsr.over(res)}


def test_check_equal_passes_and_lower_passes():
    base = _base()
    out = io.StringIO()
    assert fsr.check(fsr.measure({"mclib/a.cpp": lines(2000), "code/b.cpp": lines(100)}), base, out) == 0
    assert fsr.check(fsr.measure({"mclib/a.cpp": lines(1200), "mclib/a_diag.cpp": lines(800),
                                  "code/b.cpp": lines(100)}), base, out) == 0


def test_check_max_lines_rise_fails():
    out = io.StringIO()
    assert fsr.check(fsr.measure({"mclib/a.cpp": lines(2001), "code/b.cpp": lines(100)}), _base(), out) == 1
    assert "max_lines" in out.getvalue()


def test_check_new_oversized_file_fails_and_is_listed():
    out = io.StringIO()
    res = fsr.measure({"mclib/a.cpp": lines(1600), "code/b.cpp": lines(100), "code/new_big.cpp": lines(1700)})
    assert fsr.check(res, _base(), out) == 1
    assert "code/new_big.cpp" in out.getvalue() and "files_over_1500" in out.getvalue()


def test_check_one_dir_rising_fails_even_if_other_dir_falls():
    base = _base(b=1600)                                   # code has one big file, mclib has one
    res = fsr.measure({"mclib/a.cpp": lines(1900), "mclib/a2.cpp": lines(1600), "code/b.cpp": lines(10)})
    out = io.StringIO()
    assert fsr.check(res, base, out) == 1


def test_check_files_over_3000_rise_fails():
    base = _base(a=2900)
    out = io.StringIO()
    assert fsr.check(fsr.measure({"mclib/a.cpp": lines(3100), "code/b.cpp": lines(100)}), base, out) == 1
    assert "files_over_3000" in out.getvalue()


def test_check_growth_of_baselined_file_below_dir_max_only_warns():
    base = _base()
    res = fsr.measure({"mclib/a.cpp": lines(2000), "mclib/other.cpp": lines(10), "code/b.cpp": lines(100)})
    assert fsr.check(res, base, io.StringIO()) == 0
    # a second baselined file growing while the dir max is unchanged is only a warning
    base2 = _base()
    base2["files_over_1500"]["mclib/second.cpp"] = 1600
    base2["per_dir"]["mclib"]["files_over_1500"] = 2
    res2 = fsr.measure({"mclib/a.cpp": lines(2000), "mclib/second.cpp": lines(1650), "code/b.cpp": lines(100)})
    out = io.StringIO()
    assert fsr.check(res2, base2, out) == 0
    assert "WARNING" in out.getvalue() and "mclib/second.cpp" in out.getvalue()


# ---- end to end against a throwaway git repo: HEAD is the source of truth
@pytest.fixture
def repo(tmp_path):
    _git(tmp_path, "init", "-q")
    put(tmp_path, "mclib/big.cpp", 2000)
    put(tmp_path, "mclib/small.cpp", 100)
    put(tmp_path, "code/gen.gen.h", 5000)                  # generated: ignored
    put(tmp_path, "shaders/spv/x.frag", 5000)              # compiled output: ignored
    put(tmp_path, "mclib/3rdparty/v.cpp", 5000)            # vendored: ignored
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "file_size_ratchet.exclude").write_text("# none\n")
    commit(tmp_path, "init", ".")
    return tmp_path


def _common(repo):
    return ["--repo", str(repo), "--baseline", str(repo / "base.json")]


def test_cli_baseline_roundtrip_and_ignored_files(repo):
    c = _common(repo)
    assert fsr.main(["--write-baseline"] + c) == 0
    saved = json.load(open(repo / "base.json"))
    assert saved["total"]["files_over_1500"] == 1 and saved["total"]["max_lines"] == 2000
    assert list(saved["files_over_1500"]) == ["mclib/big.cpp"]
    assert saved["top"][0] == {"file": "mclib/big.cpp", "lines": 2000}
    assert fsr.main(["--check"] + c) == 0


def test_cli_baseline_rise_fails_and_worktree_does_not_flap_head(repo):
    c = _common(repo)
    fsr.main(["--write-baseline"] + c)
    put(repo, "mclib/big.cpp", 2100)                       # uncommitted growth
    assert fsr.main(["--check"] + c) == 0                  # HEAD unchanged
    assert fsr.main(["--check", "--worktree"] + c) == 1
    commit(repo, "grow", "mclib/big.cpp")
    assert fsr.main(["--check"] + c) == 1


def test_cli_new_oversized_file_fails(repo, capsys):
    c = _common(repo)
    fsr.main(["--write-baseline"] + c)
    put(repo, "code/fresh.cpp", 1600)
    commit(repo, "new giant", "code/fresh.cpp")
    capsys.readouterr()
    assert fsr.main(["--check"] + c) == 1
    assert "code/fresh.cpp" in capsys.readouterr().out


def test_cli_split_that_lowers_passes_and_tighten_lowers_baseline(repo):
    c = _common(repo)
    fsr.main(["--write-baseline"] + c)
    put(repo, "mclib/big.cpp", 1200)
    put(repo, "mclib/big_diag.cpp", 800)
    commit(repo, "split", "mclib/big.cpp", "mclib/big_diag.cpp")
    assert fsr.main(["--check", "--tighten"] + c) == 0
    saved = json.load(open(repo / "base.json"))
    assert saved["total"]["files_over_1500"] == 0 and saved["total"]["max_lines"] == 1200
    assert saved["files_over_1500"] == {}
    # the tightened baseline now refuses the file growing back over the line
    put(repo, "mclib/big.cpp", 1501)
    commit(repo, "regrow", "mclib/big.cpp")
    assert fsr.main(["--check"] + c) == 1


def test_cli_exclude_file_hides_a_path(repo):
    c = _common(repo)
    fsr.main(["--write-baseline"] + c)
    put(repo, "code/vendored_table.cpp", 4000)
    commit(repo, "giant", "code/vendored_table.cpp")
    assert fsr.main(["--check"] + c) == 1
    (repo / "tools" / "file_size_ratchet.exclude").write_text("# reason: machine-written table\ncode/vendored_table.cpp\n")
    commit(repo, "exclude", "tools/file_size_ratchet.exclude")
    assert fsr.main(["--check"] + c) == 0


def test_cli_missing_baseline_is_usage_error(repo):
    assert fsr.main(["--check", "--repo", str(repo), "--baseline", str(repo / "nope.json")]) == 2

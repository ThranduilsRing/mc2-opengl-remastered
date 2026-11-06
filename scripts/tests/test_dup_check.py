"""Tests for tools/dup_check.py (no PMD needed: CPD output is a fixture, the trees come from a throwaway git repo).

Run:
  py -3 -B -m pytest scripts/tests/test_dup_check.py -q -p no:cacheprovider
"""
import importlib.util
import io
import os
import subprocess
import sys
import tarfile

import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_spec = importlib.util.spec_from_file_location("dup_check", os.path.join(ROOT, "tools", "dup_check.py"))
dc = importlib.util.module_from_spec(_spec)
sys.modules["dup_check"] = dc
_spec.loader.exec_module(dc)

CSV = "lines,tokens,occurrences,file1,line1,file2,line2\n33,1344,2,a.h,10,b.h,20\n12,70,2,c.cpp,1,d.cpp,5\n"


def test_parse_csv_sums_duplicated_lines():
    assert dc.parse_csv(CSV) == (2, 45)
    assert dc.parse_csv("") == (0, 0)


def test_verdict_fails_only_on_a_rise():
    assert dc.verdict((5, 100), (5, 100))[0] == 0
    assert dc.verdict((5, 100), (4, 90))[0] == 0
    code, msg = dc.verdict((5, 100), (6, 101))
    assert code == 1 and "+1 lines" in msg and "RISE" in msg


def test_keep_member_scopes_to_owned_cpp_and_drops_generated():
    assert dc.keep_member("mclib/a.cpp") and dc.keep_member("RenderCore/x.h")
    assert not dc.keep_member("3rdparty/a.cpp")
    assert not dc.keep_member("mclib/readme.md")
    assert not dc.keep_member("RenderCore/gpu_abi/frame.gen.h")


def _git(repo, *a):
    return subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, check=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def test_filtered_tar_is_the_committed_owned_cpp_only(tmp_path):
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "config", "user.email", "t@t")
    _git(tmp_path, "config", "user.name", "t")
    (tmp_path / "mclib").mkdir()
    (tmp_path / "3rdparty").mkdir()
    (tmp_path / "mclib" / "a.cpp").write_text("int a;\n")
    (tmp_path / "mclib" / "b.gen.h").write_text("int b;\n")
    (tmp_path / "3rdparty" / "c.cpp").write_text("int c;\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-q", "-m", "x")
    (tmp_path / "mclib" / "dirty.cpp").write_text("int d;\n")  # uncommitted: must not appear
    names = tarfile.open(fileobj=io.BytesIO(dc.filtered_tar("HEAD", str(tmp_path)))).getnames()
    assert names == ["mclib/a.cpp"]


def test_missing_pmd_everywhere_is_infra_not_a_pass(monkeypatch, capsys):
    monkeypatch.setattr(dc, "have_local_pmd", lambda: None)
    monkeypatch.setattr(dc, "resolve_base", lambda *a, **k: "abc123")

    def boom(*a, **k):
        raise RuntimeError("ssh: unreachable")
    monkeypatch.setattr(dc, "measure", boom)
    assert dc.main(["--check"]) == 3
    assert "INFRA" in capsys.readouterr().err


def test_resolve_base_at_the_gate_uses_the_last_full_gate_head(tmp_path, monkeypatch):
    # On nifty itself merge-base(tip, nifty) == tip, so the gate row compared the tip with itself (delta +0 forever).
    _git(tmp_path, "init", "-q", "-b", "nifty")
    _git(tmp_path, "config", "user.email", "t@t")
    _git(tmp_path, "config", "user.name", "t")
    monkeypatch.setattr(dc, "DEFAULT_BASE", "nifty")
    shas = []
    for i in range(3):
        (tmp_path / "f.txt").write_text(str(i))
        _git(tmp_path, "add", "-A")
        _git(tmp_path, "commit", "-q", "-m", str(i))
        shas.append(_git(tmp_path, "rev-parse", "HEAD"))
    repo = str(tmp_path)
    assert dc.resolve_base(None, "HEAD", repo) == shas[2]                  # no mark: unchanged
    (tmp_path / "build64-ninja").mkdir()
    (tmp_path / "build64-ninja" / "last-full-gate.json").write_text('{"head": "%s"}' % shas[0])
    assert dc.resolve_base(None, "HEAD", repo) == shas[0]                  # gate on nifty: last published full gate
    assert dc.resolve_base("HEAD~1", "HEAD", repo) == shas[1]              # explicit --base wins
    _git(tmp_path, "checkout", "-q", "-b", "lane", shas[1])
    (tmp_path / "f.txt").write_text("lane")
    _git(tmp_path, "commit", "-qam", "lane")
    assert dc.resolve_base(None, "lane", repo) == shas[1]                  # branch: merge-base default kept

"""Tests for tools/tidy/run_tidy.py: guard selection and mode refusals (no clang-tidy needed).

Run:
  py -3 -B -m pytest tools/tidy/test_run_tidy.py -q -p no:cacheprovider
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy as rt  # noqa: E402
import run_tidy_safe as guards  # noqa: E402


def abs_(rel):
    return os.path.normpath(os.path.join(rt.ROOT, rel))


def test_select_drops_excluded_and_outside_repo(monkeypatch):
    monkeypatch.setattr(guards, "LANE_FILES", set(), raising=False)
    db = [abs_("mclib/a.cpp"), abs_("3rdparty/x/y.cpp"), abs_("build64-ninja/gen.cpp")]
    kept, skipped = rt.select(guards, [], [], db, fix=False)
    assert kept == [abs_("mclib/a.cpp")]
    assert {r for r, _ in skipped} == {"3rdparty/x/y.cpp", "build64-ninja/gen.cpp"}


def test_select_dir_and_file_filters(monkeypatch):
    monkeypatch.setattr(guards, "LANE_FILES", set(), raising=False)
    db = [abs_("mclib/a.cpp"), abs_("code/b.cpp")]
    assert rt.select(guards, [], ["mclib"], db, fix=False)[0] == [abs_("mclib/a.cpp")]
    kept, skipped = rt.select(guards, [abs_("code/b.cpp"), abs_("code/missing.cpp")], [], db, fix=False)
    assert kept == [abs_("code/b.cpp")]
    assert [r for r, _ in skipped] == ["code/missing.cpp"]


def test_select_fix_refuses_dirty_files(monkeypatch):
    monkeypatch.setattr(guards, "LANE_FILES", set(), raising=False)
    monkeypatch.setattr(guards, "dirty_files", lambda: {"mclib/a.cpp"})
    kept, skipped = rt.select(guards, [], ["mclib"], [abs_("mclib/a.cpp"), abs_("mclib/b.cpp")], fix=True)
    assert kept == [abs_("mclib/b.cpp")] and skipped == [("mclib/a.cpp", "has uncommitted changes")]


def test_fix_refuses_checks_outside_the_safe_set_and_repo_wide_runs(capsys):
    assert rt.main(["--fix", "--dir", "mclib", "--checks", "bugprone-use-after-move"]) == 2
    assert "not in the safe set" in capsys.readouterr().err


def test_not_linux_is_infra_never_a_pass(monkeypatch, capsys):
    monkeypatch.setattr(sys, "platform", "win32")
    assert rt.main([]) == 3
    assert "INFRA" in capsys.readouterr().err

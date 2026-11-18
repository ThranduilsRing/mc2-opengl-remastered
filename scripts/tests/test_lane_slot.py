"""lane_slot slot selection / release and the fixed sccache base-dir list."""
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import configure_ninja  # noqa: E402
import lane_slot  # noqa: E402


def info(n, exists=True, clean=True, claimed=False, merged=True, branch=None):
    return {"path": lane_slot.SLOT_PATHS[n - 1], "exists": exists, "clean": clean, "claimed": claimed, "merged": merged, "branch": branch}


def test_picks_first_free_skipping_dirty_claimed_unmerged():
    infos = [info(1, clean=False), info(2, claimed=True), info(3, merged=False, branch="x"), info(4)] + [info(n, exists=False) for n in range(5, 13)]
    assert lane_slot.pick_slot(infos) == (lane_slot.SLOT_PATHS[3], False)


def test_creates_next_slot_when_none_free():
    infos = [info(1, clean=False)] + [info(n, exists=False) for n in range(2, 13)]
    assert lane_slot.pick_slot(infos) == (lane_slot.SLOT_PATHS[1], True)


def test_cap_at_twelve():
    assert lane_slot.MAX_SLOTS == 12
    assert lane_slot.pick_slot([info(n, claimed=True) for n in range(1, 13)]) is None


def test_release_refuses_dirty_and_non_slot(monkeypatch):
    monkeypatch.setattr(lane_slot, "slot_info", lambda p, c: info(1, clean=False, branch="b"))
    monkeypatch.setattr(lane_slot.os.path, "isdir", lambda p: True)
    with pytest.raises(SystemExit, match="uncommitted"):
        lane_slot.release(lane_slot.SLOT_PATHS[0])
    with pytest.raises(SystemExit, match="not a slot"):
        lane_slot.release("A:/Games/mc2-main")


def test_install_dirs_never_protected():
    assert lane_slot.install_dirs("claude/foo") == ["E:/mc2-installs/claude/foo", "E:/mc2-installs/foo"]
    assert lane_slot.install_dirs("owner-test-1") == []


def test_install_is_keyed_by_slot_number():
    assert lane_slot.slot_install("A:/Games/mc2-slot02") == "E:/mc2-installs/pc-lane-02"
    assert lane_slot.slot_install("A:\\Games\\mc2-slot12") == "E:/mc2-installs/pc-lane-12"


def test_slot_info_merge_rules(monkeypatch, tmp_path):
    calls = {"merge-base": 1, "rev-parse": 1}
    fake = {"status": (0, "", ""), "symbolic-ref": (0, "claude/x", "")}
    monkeypatch.setattr(lane_slot, "git", lambda root, *a: fake.get(a[0]) or (calls[a[0]], "", ""))
    i = lane_slot.slot_info(str(tmp_path), set())
    assert i["clean"] and not i["merged"] and not lane_slot.is_free(i)
    calls["merge-base"] = 0
    assert lane_slot.is_free(lane_slot.slot_info(str(tmp_path), set()))
    assert not lane_slot.is_free(lane_slot.slot_info(str(tmp_path), {lane_slot.norm(str(tmp_path))}))


def test_fixed_root_list():
    r = configure_ninja.SCCACHE_FIXED_ROOTS
    assert "A:/Games/mc2-main" in r and "E:/mc2-installs/_precheck/tree" in r
    assert all(f"A:/Games/mc2-slot{n:02d}" in r for n in range(1, 13))
    env = configure_ninja.compile_cache_env("A:/Games/mc2-whatever")
    assert env["SCCACHE_BASEDIRS"].split(";") == r  # constant, independent of the root


def test_foreign_root_warns(monkeypatch, capsys):
    class R:
        stdout = ""
        returncode = 0
    monkeypatch.setattr(configure_ninja.subprocess, "run", lambda *a, **k: R())
    monkeypatch.setattr(configure_ninja.os, "name", "posix")
    configure_ninja.ensure_cache_server("A:/Games/mc2-lane9", {"SCCACHE_DIR": "G:/x"})
    assert "tools/lane_slot.py" in capsys.readouterr().out
    configure_ninja.ensure_cache_server("A:/Games/mc2-slot03", {"SCCACHE_DIR": "G:/x"})
    assert "WARNING" not in capsys.readouterr().out


def test_launcher_remaps_any_foreign_root(tmp_path, monkeypatch):
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import cc_launcher  # noqa: E402
    monkeypatch.delenv("SCCACHE_BASEDIRS", raising=False)
    own, foreign = tmp_path / "own", tmp_path / "wt" / "lane-x"
    nested = own / ".claude" / "worktrees" / "x"
    for base in (own, foreign, nested):
        (base / "code").mkdir(parents=True)
        (base / "code" / "x.h").write_text("x")
    sdk = tmp_path / "sdk" / "um" / "windows.h"
    sdk.parent.mkdir(parents=True)
    sdk.write_text("x")
    own_x = own / "code" / "x.h"
    assert cc_launcher.remap(str(foreign / "code" / "x.h"), str(own), []) == str(own_x)
    # a path under own but not own + separator (sibling prefix) is foreign, not own
    sib = tmp_path / "own-old" / "code"
    sib.mkdir(parents=True)
    (sib / "x.h").write_text("x")
    assert cc_launcher.remap(str(sib / "x.h"), str(own), []) == str(own_x)
    # nested worktree inside own is under own: kept
    assert cc_launcher.remap(str(nested / "code" / "x.h"), str(own), []) == str(nested / "code" / "x.h").replace("\\", "/")
    assert cc_launcher.remap(str(sdk), str(own), []) == str(sdk).replace("\\", "/")

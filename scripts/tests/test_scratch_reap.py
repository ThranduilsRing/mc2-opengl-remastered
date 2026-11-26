"""tools/scratch_reap.py: refuses the archive, owner-test, live worktrees, paths outside the roots and unmirrored crash evidence;
a dry run deletes nothing; --apply deletes only an allowed, safe slot scratch / legacy dir."""
import os
import sys
from pathlib import Path

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import scratch_reap as sr  # noqa: E402


@pytest.fixture
def env(tmp_path, monkeypatch):
    e, g, arc = tmp_path / "E-mc2-tmp", tmp_path / "G-mc2-tmp", tmp_path / "mc2-archive-2026-10"
    for d in (e, g, arc / "crashes"):
        d.mkdir(parents=True)
    monkeypatch.setattr(sr, "ROOTS", (str(e).replace("\\", "/"), str(g).replace("\\", "/")))
    monkeypatch.setattr(sr, "ARCHIVE", str(arc).replace("\\", "/"))
    monkeypatch.setattr(sr, "CRASH_MIRROR", str(arc / "crashes").replace("\\", "/"))
    monkeypatch.setattr(sr, "FORBIDDEN_PREFIXES", (str(arc).replace("\\", "/"),))
    monkeypatch.setattr(sr, "LEGACY_PARENT", str(tmp_path).replace("\\", "/") + "/")
    monkeypatch.setattr(sr, "LEGACY", ("s05",))
    monkeypatch.setattr(sr, "worktree_paths", lambda: set())
    monkeypatch.setattr(sr, "live_slots", lambda: set())
    monkeypatch.setattr(sr, "legacy_owner", lambda p: ("slot05", "slot05 test"))
    return tmp_path, e, g, arc


def slot_dir(root, name="slot05"):
    d = root / name
    (d / "sub").mkdir(parents=True)
    (d / "sub" / "f.bin").write_bytes(b"x" * 100)
    return d


def test_refuses_archive_owner_test_and_outside_roots(env):
    tmp, e, g, arc = env
    victim = arc / "x"
    victim.mkdir()
    owner = e / "owner-test-1"
    owner.mkdir()
    outside = tmp / "elsewhere"
    outside.mkdir()
    for p, word in ((victim, "protected"), (owner, "owner-test"), (outside, "outside")):
        n, why = sr.reap_path(str(p), apply=True)
        assert n == 0 and word in why and p.exists()


def test_refuses_live_worktree_and_live_lane(env, monkeypatch):
    tmp, e, g, arc = env
    d = slot_dir(e)
    monkeypatch.setattr(sr, "worktree_paths", lambda: {sr.lower(str(d))})
    assert "worktree" in sr.reap_path(str(d), apply=True)[1] and d.exists()
    monkeypatch.setattr(sr, "worktree_paths", lambda: set())
    monkeypatch.setattr(sr, "live_slots", lambda: {"slot05"})
    assert "live lane" in sr.reap_path(str(d), apply=True)[1] and d.exists()
    assert sr.reap_path(str(d), apply=True, released_slot="slot05")[1] is None and not d.exists()  # release drops the claim first


def test_dry_run_deletes_nothing_apply_deletes(env):
    tmp, e, g, arc = env
    d = slot_dir(e)
    assert sr.main(["slot05"]) == 0 and d.exists()
    assert sr.main(["slot05", "--dry-run"]) == 0 and d.exists()
    n, why = sr.reap_path(str(d), apply=False)
    assert n == 100 and why is None and (d / "sub" / "f.bin").exists()
    assert sr.main(["slot05", "--apply"]) == 0 and not d.exists()


def test_unmirrored_crash_evidence_is_kept_mirrored_is_not(env):
    tmp, e, g, arc = env
    d = slot_dir(e)
    (d / "crashes").mkdir()
    dump = d / "crashes" / "a.dmp"
    dump.write_bytes(b"d" * 7)
    n, why = sr.reap_path(str(d), apply=True)
    assert "not mirrored" in why and dump.exists()
    (arc / "crashes" / "x-2026").mkdir()
    (arc / "crashes" / "x-2026" / "a.dmp").write_bytes(b"d" * 7)
    assert sr.reap_path(str(d), apply=True)[1] is None and not d.exists()


def test_legacy_only_allow_listed(env):
    tmp, e, g, arc = env
    ok, bad = tmp / "mc2-tmp-s05", tmp / "mc2-tmp-zzz"
    for d in (ok, bad):
        d.mkdir()
    assert sr.main(["--legacy", str(bad), "--apply"]) == 2 and bad.exists()
    assert sr.main(["--legacy", str(ok), "--apply"]) == 0 and not ok.exists()


def test_ensure_creates_only_g_scratch(env, monkeypatch):
    _, e, g, _ = env
    monkeypatch.setattr(sr, "SCRATCH_ROOT", str(g).replace("\\", "/"))
    sr.ensure_slot_scratch("slot05")
    assert (g / "slot05").is_dir()
    assert not (e / "slot05").exists()

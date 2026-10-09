#!/usr/bin/env python3
# scripts/tests/test_fingerprint.py
"""Tests for scripts/smoke_lib/fingerprint.py (deploy-fingerprint parse/compare).

Pure functions only -- no game launch, no git, no filesystem.

Run:
  py -3 -m pytest scripts/tests/test_fingerprint.py -v
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib.fingerprint import parse_fingerprint, check_fingerprint

BANNER = ("[BUILD_FINGERPRINT v1] sha=0123456789ab dirty=0 "
          "branch=claude/sp-deploy-fingerprint-1 built=2026-06-11T12:00:00 "
          "src=A:/Games/mc2-opengl-src/.claude/worktrees/sp-deploy-fingerprint-1")
LOG = "noise line\n" + BANNER + "\n[exit] gos_TerminateApplication called\n"


def test_parse_ok():
    fp = parse_fingerprint(LOG)
    assert fp is not None
    assert fp["sha"] == "0123456789ab"
    assert fp["dirty"] == 0
    assert fp["branch"] == "claude/sp-deploy-fingerprint-1"
    assert fp["built"] == "2026-06-11T12:00:00"
    assert fp["src"].endswith("sp-deploy-fingerprint-1")


def test_parse_dirty_flag():
    fp = parse_fingerprint(LOG.replace("dirty=0", "dirty=1"))
    assert fp["dirty"] == 1


def test_parse_absent():
    assert parse_fingerprint("no banner here\n") is None
    assert parse_fingerprint("") is None


def test_check_match():
    fp = parse_fingerprint(LOG)
    lines, hard_fail = check_fingerprint(fp, "0123456789abcdef0123456789abcdef01234567")
    assert not hard_fail
    assert any("OK" in l for l in lines)


def test_check_mismatch_advisory_text():
    fp = parse_fingerprint(LOG)
    lines, hard_fail = check_fingerprint(fp, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
    assert hard_fail  # caller enforces only under MC2_SMOKE_REQUIRE_FINGERPRINT=1
    assert any(l.startswith("[DEPLOY_FINGERPRINT] MISMATCH: exe sha=0123456789ab "
                            "expected=deadbeefdead") for l in lines)


def test_check_absent_is_advisory_hard_fail():
    lines, hard_fail = check_fingerprint(None, "0123456789ab")
    assert hard_fail
    assert any("fingerprint absent (pre-fingerprint exe)" in l for l in lines)


def test_check_dirty_warning_passthrough():
    fp = parse_fingerprint(LOG.replace("dirty=0", "dirty=1"))
    lines, hard_fail = check_fingerprint(fp, "0123456789ab")
    assert not hard_fail  # dirty alone is not a mismatch
    assert any("BUILT-FROM-DIRTY-TREE" in l for l in lines)


def test_check_no_expected_sha():
    fp = parse_fingerprint(LOG)
    lines, hard_fail = check_fingerprint(fp, None)
    assert not hard_fail
    assert any("expected sha unavailable" in l for l in lines)


def test_check_case_insensitive_sha():
    fp = parse_fingerprint(LOG.replace("0123456789ab", "0123456789AB"))
    lines, hard_fail = check_fingerprint(fp, "0123456789ABCDEF")
    assert not hard_fail


# --- ancestor acceptance: exe built at an earlier commit, only non-build files since ---

from scripts.smoke_lib.fingerprint import is_build_input

HEAD = "f" * 40


def _fake_git(diff_files, ancestor=True, count="3"):
    def run(_root, *args):
        if args[0] == "merge-base":
            return "" if ancestor else None
        if args[0] == "diff":
            return "\n".join(diff_files)
        if args[0] == "rev-list":
            return count
        return None
    return run


def test_ancestor_doc_only_commits_ok():
    fp = parse_fingerprint(LOG)
    lines, hard = check_fingerprint(fp, HEAD, repo_root=".",
                                    git=_fake_git(["docs/a.md", ".claude/x.md", "scripts/run_smoke.py"]))
    assert not hard
    assert any(l == "[DEPLOY_FINGERPRINT] OK (ancestor 0123456789ab, 3 doc-only commits since)"
               for l in lines)


def test_ancestor_build_input_changed_fails():
    fp = parse_fingerprint(LOG)
    lines, hard = check_fingerprint(fp, HEAD, repo_root=".",
                                    git=_fake_git(["docs/a.md", "mclib/terrain.cpp"]))
    assert hard
    assert any("build inputs changed since: mclib/terrain.cpp" in l for l in lines)


def test_ancestor_not_ancestor_fails():
    fp = parse_fingerprint(LOG)
    _lines, hard = check_fingerprint(fp, HEAD, repo_root=".",
                                     git=_fake_git(["docs/a.md"], ancestor=False))
    assert hard


def test_ancestor_dirty_exe_fails():
    fp = parse_fingerprint(LOG.replace("dirty=0", "dirty=1"))
    _lines, hard = check_fingerprint(fp, HEAD, repo_root=".", git=_fake_git(["docs/a.md"]))
    assert hard


def test_no_repo_root_keeps_exact_match_only():
    fp = parse_fingerprint(LOG)
    _lines, hard = check_fingerprint(fp, HEAD)
    assert hard


def test_build_input_classification():
    for p in ("CMakeLists.txt", "cmake/Mc2NarrowingErrors.cmake", "shaders/mech.vert",
              "code/objmgr.cpp", "tools/gpu_abi_gen.py", "tools/mc2_launcher/mc2_launcher.cpp",
              "RenderCore/KtxLoader.cpp", "3rdparty/vma/x.h", "editor/EditorObjects.cpp"):
        assert is_build_input(p), p
    for p in ("docs/x.md", ".claude/cleanup/a.md", "scripts/run_smoke.py", "tools/repo_intel/x.py",
              "memory/m.md", "README.md", "scripts/gl_shader_variants.json"):
        assert not is_build_input(p), p

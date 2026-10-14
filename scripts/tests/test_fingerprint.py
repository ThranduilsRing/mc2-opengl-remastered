#!/usr/bin/env python3
# scripts/tests/test_fingerprint.py
"""Tests for scripts/smoke_lib/fingerprint.py (deploy-fingerprint parse/compare).

No game launch or git; executable hashing uses in-memory byte streams.

Run:
  py -3 -m pytest scripts/tests/test_fingerprint.py -v
"""
from __future__ import annotations

import sys
import hashlib
from io import BytesIO
from pathlib import Path
from unittest.mock import patch

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib.fingerprint import (parse_fingerprint, check_fingerprint,
                                          resolve_reference, describe_reference)

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
    assert hard_fail
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


def test_shared_build_inputs_file_loads():
    from scripts.smoke_lib.fingerprint import load_build_inputs
    prefixes, files = load_build_inputs()
    assert "code/" in prefixes and "mclib/" in prefixes and "cmake/" in prefixes
    assert "CMakeLists.txt" in files and "tools/gpu_abi_gen.py" in files
    assert not any(p.startswith("#") for p in prefixes + files)
    assert not any(p.startswith(("docs/", ".claude/", "scripts/")) for p in prefixes + files)


COMMIT = "0123456789ab" + "c" * 28


def test_default_reference_resolves_head():
    with patch("scripts.smoke_lib.fingerprint._git", return_value=COMMIT) as git:
        ref = resolve_reference(".", git=git)
    git.assert_called_once_with(".", "rev-parse", "--verify", "HEAD^{commit}")
    assert ref == {"mode": "head", "requested": "HEAD", "sha": COMMIT}
    assert "worktree HEAD" in describe_reference(ref)


def test_commit_reference_resolves_and_matches_old_build():
    with patch("scripts.smoke_lib.fingerprint._git", return_value=COMMIT) as git:
        ref = resolve_reference(".", ref_commit="0123456789ab", git=git)
    git.assert_called_once_with(".", "rev-parse", "--verify", "0123456789ab^{commit}")
    lines, bad = check_fingerprint(parse_fingerprint(LOG), HEAD, reference=ref)
    assert not bad
    assert "--ref-commit 0123456789ab" in lines[-1]
    assert "worktree HEAD" not in lines[-1]


@pytest.mark.parametrize("parsed", [None, {"sha": "0" * 12, "dirty": 0},
                                   {"sha": COMMIT[:12], "dirty": 1}])
def test_commit_reference_rejects_absent_wrong_or_dirty(parsed):
    ref = {"mode": "commit", "requested": COMMIT, "sha": COMMIT}
    _lines, bad = check_fingerprint(parsed, None, reference=ref)
    assert bad


@pytest.mark.parametrize("changed", [["docs/a.md"], ["code/a.cpp"]])
def test_commit_reference_rejects_different_ancestors(changed):
    ref = {"mode": "commit", "requested": HEAD, "sha": HEAD}
    with patch("scripts.smoke_lib.fingerprint._git", wraps=_fake_git(changed)) as git:
        lines, bad = check_fingerprint(parse_fingerprint(LOG), None, reference=ref,
                                       repo_root=".", git=git)
    assert bad
    assert "MISMATCH" in lines[-1]
    git.assert_not_called()


def test_head_reference_keeps_build_equivalent_ancestors():
    ref = {"mode": "head", "requested": "HEAD", "sha": HEAD}
    _lines, bad = check_fingerprint(parse_fingerprint(LOG), None, reference=ref,
                                   repo_root=".", git=_fake_git(["docs/a.md"]))
    assert not bad
    _lines, bad = check_fingerprint(parse_fingerprint(LOG), None, reference=ref,
                                   repo_root=".", git=_fake_git(["code/a.cpp"]))
    assert bad


@pytest.mark.parametrize("value", ["HEAD", "", "-option", "abcd", "a" * 41])
def test_commit_reference_rejects_non_sha(value):
    with pytest.raises(ValueError, match="commit SHA"):
        resolve_reference(".", ref_commit=value)


def test_commit_reference_must_exist():
    with pytest.raises(ValueError, match="cannot resolve"):
        resolve_reference(".", ref_commit="deadbeef", git=lambda *_: None)


def test_reference_modes_are_mutually_exclusive():
    with pytest.raises(ValueError, match="mutually exclusive"):
        resolve_reference(".", ref_commit=COMMIT, ref_exe="pre.exe")


def test_byte_identical_exe_reference_passes():
    payload = b"deliberately older executable bytes"
    with patch.object(Path, "open", side_effect=lambda *_: BytesIO(payload)):
        ref = resolve_reference(".", ref_exe="pre.exe")
        lines, bad = check_fingerprint(parse_fingerprint(LOG), HEAD,
                                       reference=ref, exe_path="deployed.exe")
    assert not bad
    assert ref["sha256"] == hashlib.sha256(payload).hexdigest()
    assert "--ref-exe" in lines[-1] and "byte identity" in lines[-1]


def test_different_bytes_same_embedded_commit_fails():
    with patch.object(Path, "open", side_effect=[BytesIO(b"pre"), BytesIO(b"post")]):
        ref = resolve_reference(".", ref_exe="pre.exe")
        lines, bad = check_fingerprint(parse_fingerprint(LOG), None,
                                       reference=ref, exe_path="deployed.exe")
    assert bad
    assert "MISMATCH" in lines[-1] and "sha256=" in lines[-1]


def test_exe_reference_still_requires_fingerprint():
    with patch.object(Path, "open", return_value=BytesIO(b"pre")):
        ref = resolve_reference(".", ref_exe="pre.exe")
    _lines, bad = check_fingerprint(None, None, reference=ref, exe_path="pre.exe")
    assert bad


def test_missing_reference_exe_aborts():
    with patch.object(Path, "open", side_effect=FileNotFoundError("missing")):
        with pytest.raises(ValueError, match="cannot read --ref-exe"):
            resolve_reference(".", ref_exe="pre.exe")


def test_unreadable_deployed_exe_fails():
    ref = {"mode": "exe", "path": "pre.exe", "sha256": "a" * 64}
    with patch.object(Path, "open", side_effect=PermissionError("denied")):
        lines, bad = check_fingerprint(parse_fingerprint(LOG), None,
                                       reference=ref, exe_path="deployed.exe")
    assert bad and "cannot hash deployed exe" in lines[-1]


def test_exe_reference_pins_bytes_even_if_reference_changes():
    with patch.object(Path, "open", side_effect=[BytesIO(b"pre"), BytesIO(b"post")]):
        ref = resolve_reference(".", ref_exe="pre.exe")
        _lines, bad = check_fingerprint(parse_fingerprint(LOG), None,
                                        reference=ref, exe_path="pre.exe")
    assert bad


def test_exe_reference_can_identify_dirty_build_by_bytes():
    with patch.object(Path, "open", side_effect=lambda *_: BytesIO(b"dirty build")):
        ref = resolve_reference(".", ref_exe="pre.exe")
        _lines, bad = check_fingerprint(parse_fingerprint(LOG.replace("dirty=0", "dirty=1")),
                                        None, reference=ref, exe_path="deployed.exe")
    assert not bad

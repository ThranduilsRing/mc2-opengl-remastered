# scripts/smoke_lib/fingerprint.py
"""Deploy-fingerprint parsing/compare for run_smoke.

The exe prints (code/mechcmd2.cpp InitializeGameEngine, build-time stamped by
cmake/GenerateBuildFingerprint.cmake):

  [BUILD_FINGERPRINT v1] sha=<12-hex> dirty=<0|1> branch=<name> built=<iso> src=<worktree>

run_smoke compares the exe's sha against the worktree HEAD it runs from.
DEFAULT = HARD FAIL on mismatch/absence (DEPLOY-FINGERPRINT-DEFAULT-ON-1): a
smoke that ran a stale or wrong-lane exe must never print PASS. Opt OUT with
MC2_SMOKE_REQUIRE_FINGERPRINT=0 for the narrow case of deliberately smoking an
external prebuilt exe.

Pure functions, no subprocess/game launch — unit-tested in
scripts/tests/test_fingerprint.py.
"""
from __future__ import annotations

import re
import subprocess
from typing import Callable, Optional, Sequence

FINGERPRINT_RE = re.compile(
    r"\[BUILD_FINGERPRINT v1\] "
    r"sha=(?P<sha>[0-9a-fA-F]+) "
    r"dirty=(?P<dirty>[01]) "
    r"branch=(?P<branch>\S+) "
    r"built=(?P<built>\S+) "
    r"src=(?P<src>\S+)")


def parse_fingerprint(log_text: str) -> Optional[dict]:
    """Return {sha, dirty, branch, built, src} from the first banner line, or None."""
    m = FINGERPRINT_RE.search(log_text or "")
    if not m:
        return None
    d = m.groupdict()
    d["sha"] = d["sha"].lower()
    d["dirty"] = int(d["dirty"])
    return d


# Paths whose change alters the built exe. A commit range that touches none of
# these cannot make a prior exe stale, so an exe built at an ancestor of HEAD is
# still the HEAD exe.
BUILD_INPUT_PREFIXES = (
    "code/", "mclib/", "GameOS/", "RenderCore/", "GuiRuntime/", "RenderWorld/",
    "GameAdapters/", "HostServices/", "editor/", "gui/", "shaders/", "cmake/",
    "tools/mc2_launcher/", "3rdparty/",
)
BUILD_INPUT_FILES = (
    "CMakeLists.txt", "tools/gpu_abi_gen.py", "vcpkg.json", "resource.h",
    "strings.res.h",
)


def is_build_input(path: str) -> bool:
    p = path.replace("\\", "/")
    return p in BUILD_INPUT_FILES or p.startswith(BUILD_INPUT_PREFIXES)


def build_inputs_changed(paths: Sequence[str]) -> list[str]:
    return [p for p in paths if is_build_input(p)]


def _git(repo_root, *args) -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo_root), *args], text=True,
            stderr=subprocess.DEVNULL, timeout=30).strip()
    except Exception:
        return None


def check_ancestor_fingerprint(fp: dict, expected_sha: str, repo_root,
                               git: Callable = _git):
    """Accept an exe built at an ancestor of HEAD when nothing since touched a
    build input. Returns (lines, ok)."""
    lines: list[str] = []
    if fp.get("dirty"):
        lines.append("[DEPLOY_FINGERPRINT] MISMATCH: exe sha="
                     f"{fp['sha']} is not HEAD and was built from a dirty tree")
        return lines, False
    sha = fp["sha"]
    if git(repo_root, "merge-base", "--is-ancestor", sha, expected_sha) is None:
        lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={sha} "
                     f"expected={expected_sha[:12]} (not an ancestor of HEAD)")
        return lines, False
    diff = git(repo_root, "diff", "--name-only", f"{sha}..{expected_sha}")
    if diff is None:
        lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={sha} "
                     f"expected={expected_sha[:12]} (git diff failed)")
        return lines, False
    bad = build_inputs_changed([l for l in diff.splitlines() if l])
    if bad:
        shown = ", ".join(bad[:3]) + (f" (+{len(bad) - 3} more)" if len(bad) > 3 else "")
        lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={sha} expected="
                     f"{expected_sha[:12]}; build inputs changed since: {shown}")
        return lines, False
    count = git(repo_root, "rev-list", "--count", f"{sha}..{expected_sha}") or "?"
    lines.append(f"[DEPLOY_FINGERPRINT] OK (ancestor {sha}, {count} doc-only "
                 "commits since)")
    return lines, True


def check_fingerprint(fp: Optional[dict], expected_sha: Optional[str],
                      repo_root=None, git: Callable = _git):
    """Compare a parsed fingerprint against the runner worktree HEAD.

    Returns (lines, hard_fail). hard_fail is enforced by the caller BY DEFAULT
    (run_smoke clears its PASS verdict); MC2_SMOKE_REQUIRE_FINGERPRINT=0
    downgrades it to advisory. With repo_root, an exe built at an ancestor of
    HEAD passes when no build input changed in between.
    """
    lines: list[str] = []
    if fp is None:
        lines.append("[DEPLOY_FINGERPRINT] fingerprint absent (pre-fingerprint exe)")
        return lines, True
    if fp.get("dirty"):
        lines.append("[DEPLOY_FINGERPRINT] WARNING: exe BUILT-FROM-DIRTY-TREE "
                     f"(sha={fp['sha']} branch={fp.get('branch')})")
    if not expected_sha:
        lines.append("[DEPLOY_FINGERPRINT] expected sha unavailable (git rev-parse failed); "
                     f"exe sha={fp['sha']}")
        return lines, False
    exp = expected_sha.lower()
    n = min(len(fp["sha"]), len(exp))
    if n >= 7 and fp["sha"][:n] == exp[:n]:
        lines.append(f"[DEPLOY_FINGERPRINT] OK: exe sha={fp['sha']} matches worktree HEAD")
        return lines, False
    if repo_root is not None:
        anc_lines, ok = check_ancestor_fingerprint(fp, exp, repo_root, git)
        if ok:
            return lines + anc_lines, False
        return lines + anc_lines, True
    lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={fp['sha']} expected={exp[:12]}")
    return lines, True

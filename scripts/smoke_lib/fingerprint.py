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
from typing import Optional

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


def check_fingerprint(fp: Optional[dict], expected_sha: Optional[str]):
    """Compare a parsed fingerprint against the runner worktree HEAD.

    Returns (lines, hard_fail). hard_fail is enforced by the caller BY DEFAULT
    (run_smoke clears its PASS verdict); MC2_SMOKE_REQUIRE_FINGERPRINT=0
    downgrades it to advisory.
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
    lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={fp['sha']} expected={exp[:12]}")
    return lines, True

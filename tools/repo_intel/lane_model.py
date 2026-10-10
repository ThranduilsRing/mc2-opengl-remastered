#!/usr/bin/env python3
"""
lane_model.py: the builder-lane table (install + worktree + branch + build dir).

WHY: deploy_status compared every install's exe fingerprint against the CALLING
worktree's HEAD. That is right for the dev lane (0.4, nifty) but wrong for the
second builder lane, whose exe is built from a different worktree/branch: it
always read FINGERPRINT-MISMATCH against nifty's HEAD. A lane therefore needs to
say which worktree owns it so the "expected" side of the comparison follows.

WHAT: pure data + lookups, no I/O beyond path normalisation. The install-name
lists in scripts/smoke_lib/deploy_lease.py (smoke-runner --deploy names) and
scripts/check-deploy-target.py (GOOD_DIRS) stay the source for lanes the smoke
runner can lease; this table only ADDS the ownership facts they do not carry.
`worktree=None` means "whichever worktree the caller is in" (dev lane / golden).
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional

INSTALL_ROOT = "A:/Games/mc2-opengl"

# name -> {install, worktree, branch, build_dir}. Archived lanes (0.4d-rc1,
# 0.5.x, releases/*) live on E:/mc2-archive-2026-10 and are deliberately absent.
LANES: dict = {
    "0.4": {
        "install": f"{INSTALL_ROOT}/mc2-win64-v0.4",
        "worktree": None, "branch": None, "build_dir": None,
    },
    "0.4c": {
        "install": f"{INSTALL_ROOT}/mc2-win64-v0.4c",
        "worktree": None, "branch": None, "build_dir": None,
    },
    "lane2": {
        "install": f"{INSTALL_ROOT}/mc2-win64-lane2",
        "worktree": "A:/Games/mc2-lane2",
        "branch": "claude/lane2-s3",
        "build_dir": "A:/Games/mc2-lane2/build64-ninja",
    },
}


def _norm(p) -> str:
    return str(p).replace("\\", "/").rstrip("/").casefold()


def install_paths() -> list:
    """(name, install path) pairs, registration order."""
    return [(n, e["install"]) for n, e in LANES.items()]


def lane_for(lane_or_path: str) -> Optional[tuple]:
    """(name, entry) for a lane name or its install path; None if unregistered."""
    key = (lane_or_path or "").strip()
    if key in LANES:
        return key, LANES[key]
    nk = _norm(key)
    for name, entry in LANES.items():
        if _norm(entry["install"]) == nk:
            return name, entry
    return None


def owner_worktree(lane_or_path: str) -> Optional[Path]:
    """The worktree that builds this lane, or None (= caller's own worktree).

    Also None when the registered worktree is absent on disk, so a stale entry
    degrades to the old caller-worktree behaviour instead of erroring.
    """
    hit = lane_for(lane_or_path)
    if not hit or not hit[1]["worktree"]:
        return None
    wt = Path(hit[1]["worktree"])
    return wt if wt.is_dir() else None

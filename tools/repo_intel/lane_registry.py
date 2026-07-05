#!/usr/bin/env python3
"""
MCP-ANTI-CHURN-1: lane_registry.py
Cross-worktree lane coordination store for concurrent agent lanes.

Grounded in observed failures (2026-07-01):
  - S2 commit scooped another agent's hunks (shared-worktree cross-contamination)
  - zombie agent chains double-implementing a slice
  - two lanes deploying to the same dir -> DEPLOY_FINGERPRINT mismatch x3

A "lane" is one concurrent agent session: {name, worktree, branch, deploy_lane,
files_claimed (repo-relative globs), note}. The store is ONE shared JSON file so
every worktree of the same repo sees the same registry:

    <main-repo-root>/.claude/lane_registry.json

where main-repo-root = parent of `git rev-parse --git-common-dir` (worktrees
share the common git dir). Override with MC2_LANE_REGISTRY_PATH (tests).

STALE RULE: entries older than MC2_LANE_TTL_SECS (default 12h) since last_touch
are flagged stale and ignored for conflict verdicts. register re-touches.

Writes are coordination-metadata only — never repo content. Atomic tmp+replace
with an O_EXCL sidecar lock (same pattern as scripts/smoke_lib/deploy_lease.py).
"""
from __future__ import annotations

import contextlib
import fnmatch
import json
import os
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

_DEFAULT_TTL_SECS = 12 * 3600
_LOCK_WAIT_SECS = 5
_LOCK_STALE_SECS = 30


# ---------------------------------------------------------------------------
# Store location
# ---------------------------------------------------------------------------

def store_path(repo_root: Path) -> Path:
    """Resolve the shared registry path (env override > git common dir > local)."""
    env = os.environ.get("MC2_LANE_REGISTRY_PATH")
    if env:
        return Path(env)
    try:
        r = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--git-common-dir"],
            capture_output=True, text=True, timeout=5, stdin=subprocess.DEVNULL,
        )
        common = r.stdout.strip()
        if r.returncode == 0 and common:
            common_p = Path(common)
            if not common_p.is_absolute():
                common_p = (Path(repo_root) / common_p).resolve()
            main_root = common_p.parent  # parent of .git = main repo root
            return main_root / ".claude" / "lane_registry.json"
    except Exception:
        pass
    return Path(repo_root) / ".claude" / "lane_registry.json"


def _ttl() -> int:
    try:
        return int(os.environ.get("MC2_LANE_TTL_SECS", ""))
    except (ValueError, TypeError):
        return _DEFAULT_TTL_SECS


def _now_utc() -> str:
    return datetime.now(tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _age_secs(utc_str: str) -> Optional[float]:
    for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%dT%H:%M:%S+00:00"):
        try:
            ts = datetime.strptime(utc_str, fmt).replace(tzinfo=timezone.utc)
            return time.time() - ts.timestamp()
        except (ValueError, TypeError):
            continue
    return None


def _is_stale(entry: dict) -> bool:
    age = _age_secs(entry.get("last_touch_utc", ""))
    if age is None:
        return True
    return age > _ttl()


# ---------------------------------------------------------------------------
# Registry I/O (atomic, locked)
# ---------------------------------------------------------------------------

def _read(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, dict) and isinstance(data.get("lanes"), dict):
            return data
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        pass
    return {"schema": "mc2-lane-registry/1", "lanes": {}}


def _write(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    os.replace(str(tmp), str(path))


@contextlib.contextmanager
def _lock(path: Path):
    lp = path.with_suffix(".lock")
    lp.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + _LOCK_WAIT_SECS
    acquired = False
    while True:
        try:
            fd = os.open(str(lp), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.close(fd)
            acquired = True
            break
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(lp) > _LOCK_STALE_SECS:
                    os.unlink(str(lp))
                    continue
            except OSError:
                pass
            if time.time() >= deadline:
                break  # proceed best-effort rather than deadlock
            time.sleep(0.05)
    try:
        yield
    finally:
        if acquired:
            try:
                os.unlink(str(lp))
            except OSError:
                pass


# ---------------------------------------------------------------------------
# Path matching
# ---------------------------------------------------------------------------

def _norm(p: str) -> str:
    return p.replace("\\", "/").strip().lstrip("./").casefold()


def _paths_overlap(claim: str, path: str) -> bool:
    """True when a claimed glob/path and a proposed path collide.

    Handles: exact, glob claim vs path, path-as-glob vs claim, and directory
    prefix claims ("mclib/" claims everything under mclib/).
    """
    c, p = _norm(claim), _norm(path)
    if not c or not p:
        return False
    if c == p:
        return True
    if fnmatch.fnmatch(p, c) or fnmatch.fnmatch(c, p):
        return True
    if c.endswith("/") and p.startswith(c):
        return True
    if p.endswith("/") and c.startswith(p):
        return True
    return False


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def register_lane(repo_root: Path, name: str, worktree: str = "",
                  files_claimed: Optional[list] = None, deploy_lane: str = "",
                  note: str = "") -> dict:
    """Register (or re-touch) a lane. Returns the record + immediate conflicts."""
    name = (name or "").strip()
    if not name:
        return {"error": "lane name required"}
    path = store_path(repo_root)
    claims = [str(c) for c in (files_claimed or []) if str(c).strip()]
    with _lock(path):
        data = _read(path)
        existing = data["lanes"].get(name, {})
        rec = {
            "name": name,
            "worktree": str(worktree or existing.get("worktree") or repo_root),
            "branch": _branch_of(worktree or existing.get("worktree") or repo_root),
            "deploy_lane": deploy_lane or existing.get("deploy_lane", ""),
            "files_claimed": claims or existing.get("files_claimed", []),
            "note": note or existing.get("note", ""),
            "pid": os.getpid(),
            "registered_utc": existing.get("registered_utc") or _now_utc(),
            "last_touch_utc": _now_utc(),
        }
        data["lanes"][name] = rec
        _write(path, data)

    conflicts = check_conflict(repo_root, rec["files_claimed"], exclude_lane=name)
    deploy_clash = [
        ln for ln, e in _fresh_lanes(repo_root).items()
        if ln != name and rec["deploy_lane"] and e.get("deploy_lane") == rec["deploy_lane"]
    ]
    return {
        "registered": rec,
        "store": str(path),
        "file_conflicts": conflicts.get("conflicts", []),
        "deploy_lane_clash": deploy_clash,
        "warnings": (
            ([f"deploy lane '{rec['deploy_lane']}' also claimed by: {', '.join(deploy_clash)}"]
             if deploy_clash else [])
            + ([f"{len(conflicts.get('conflicts', []))} claimed path(s) overlap another lane"]
               if conflicts.get("conflicts") else [])
        ),
    }


def release_lane(repo_root: Path, name: str) -> dict:
    """Remove a lane entry. No-op if absent."""
    path = store_path(repo_root)
    with _lock(path):
        data = _read(path)
        removed = data["lanes"].pop(name, None)
        _write(path, data)
    return {"released": bool(removed), "name": name, "store": str(path)}


def _fresh_lanes(repo_root: Path) -> dict:
    data = _read(store_path(repo_root))
    return {n: e for n, e in data["lanes"].items() if not _is_stale(e)}


def list_lanes(repo_root: Path) -> dict:
    """All lanes with age + stale flag (stale entries are kept but flagged)."""
    path = store_path(repo_root)
    data = _read(path)
    lanes = []
    for name, e in sorted(data["lanes"].items()):
        age = _age_secs(e.get("last_touch_utc", ""))
        lanes.append({
            **e,
            "age_secs": round(age) if age is not None else None,
            "stale": _is_stale(e),
        })
    return {"store": str(path), "count": len(lanes), "lanes": lanes,
            "ttl_secs": _ttl()}


def check_conflict(repo_root: Path, paths: Optional[list],
                   exclude_lane: str = "") -> dict:
    """Which FRESH foreign lanes claim any of these paths?

    Returns {conflicts: [{path, lane, claim, worktree, deploy_lane}], clear: bool}.
    """
    paths = [str(p) for p in (paths or []) if str(p).strip()]
    conflicts = []
    for lane_name, e in _fresh_lanes(repo_root).items():
        if lane_name == exclude_lane:
            continue
        for claim in e.get("files_claimed", []):
            for p in paths:
                if _paths_overlap(claim, p):
                    conflicts.append({
                        "path": p,
                        "lane": lane_name,
                        "claim": claim,
                        "worktree": e.get("worktree"),
                        "deploy_lane": e.get("deploy_lane"),
                    })
    return {
        "checked_paths": len(paths),
        "conflicts": conflicts,
        "clear": not conflicts,
        "store": str(store_path(repo_root)),
    }


def _branch_of(worktree) -> Optional[str]:
    try:
        r = subprocess.run(
            ["git", "-C", str(worktree), "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5, stdin=subprocess.DEVNULL,
        )
        return r.stdout.strip() or None
    except Exception:
        return None

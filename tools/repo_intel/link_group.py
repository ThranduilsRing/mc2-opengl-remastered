#!/usr/bin/env python3
"""
LINK-GUARD-1: link_group.py

Answers the one question the lane registry could not: "is this install really
MINE, or am I holding the same bytes as somebody else?"

Every other coordination record in this repo is PATH-KEYED --
.mc2_deploy_lane.json, .smoke_leases.json, the lane registry's deploy_lane
string, deploy_status()'s whole report. A hardlink is exactly the case where
two paths are one object, so on 2026-07-30 four directories under
A:/Games/mc2-opengl each reported themselves free while sharing ~1530 files
(mc2.exe, the shader tree, every DLL, "Mission Editor.exe", and --
memorably -- .mc2_deploy_lane.json itself, so the lane LOCK was shared bytes
too). Two lanes could hold "separate" installs and be writing one install.

This module makes the link group a first-class field: deploy_status() reports
it, register_lane() cross-references it against the other registered lanes.

Classification is NOT restated here. It is imported from
scripts/check-hardlink-isolation.py so the MCP answer and the deploy-time
refusal can never drift into disagreeing about what counts as a written
artifact.

READ-ONLY. Never writes.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
from typing import Optional

_GUARD_CACHE = None
_GUARD_ERR: Optional[str] = None

# Where sibling installs live. Attribution only -- st_nlink, not this list, is
# what decides `isolated` (see check-hardlink-isolation.py's docstring on why
# that distinction is the whole point).
DEFAULT_INSTALL_ROOTS = (
    r"A:/Games/mc2-opengl",
)


def _load_guard(repo_root: Path):
    """Import scripts/check-hardlink-isolation.py (hyphenated) as a module.

    Only SUCCESS is cached. Caching the failure too would be cheaper, but the
    MCP server is long-lived: a server started against a checkout that did not
    yet have the guard would answer UNKNOWN forever, including after the file
    arrived. A stat per call is the cheaper mistake.
    """
    global _GUARD_CACHE, _GUARD_ERR
    if _GUARD_CACHE is not None:
        return _GUARD_CACHE
    _GUARD_ERR = None
    path = Path(repo_root) / "scripts" / "check-hardlink-isolation.py"
    if not path.is_file():
        _GUARD_ERR = f"guard not found at {path}"
        return None
    try:
        spec = importlib.util.spec_from_file_location(
            "check_hardlink_isolation", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception as e:  # noqa: BLE001
        _GUARD_ERR = f"guard unimportable: {e}"
        return None
    _GUARD_CACHE = mod
    return mod


def resolve_deploy_dir(lane: str, repo_root: Optional[Path] = None) -> Optional[Path]:
    """Turn a deploy_lane string into a directory.

    Accepts an absolute path, a canonical short name from
    smoke_lib/deploy_lease.DEPLOY_FOLDERS or lane_model.LANES ("0.4", "0.4c",
    "lane2"), or a bare directory name under the known install roots
    ("mc2-win64-lane2").
    Returns None if it cannot be resolved -- callers must treat that as
    "unknown", never as "fine".
    """
    lane = (lane or "").strip()
    if not lane:
        return None
    p = Path(lane)
    if p.is_absolute() and p.is_dir():
        return p
    if repo_root:
        try:
            import sys
            scripts = str(Path(repo_root) / "scripts")
            if scripts not in sys.path:
                sys.path.insert(0, scripts)
            from smoke_lib import deploy_lease  # type: ignore
            for name, path in deploy_lease.DEPLOY_FOLDERS:
                if name == lane and Path(path).is_dir():
                    return Path(path)
        except Exception:  # noqa: BLE001
            pass
    try:
        import lane_model  # sibling module  # noqa: PLC0415
        hit = lane_model.lane_for(lane)
        if hit and Path(hit[1]["install"]).is_dir():
            return Path(hit[1]["install"])
    except Exception:  # noqa: BLE001
        pass
    for root in DEFAULT_INSTALL_ROOTS:
        cand = Path(root) / lane
        if cand.is_dir():
            return cand
    return None


def link_group(repo_root: Path, deploy_dir) -> dict:
    """Link-group report for one deploy dir.

    Returns:
      verdict  OK | SHARED | UNKNOWN   (UNKNOWN = could not check; never "fine")
      isolated True only when the check RAN and found nothing shared.
      peers    absolute paths of the other installs holding these same inodes.
      shared_count / mutated_checked / examples / remedy / note
    """
    res = {
        "verdict": "UNKNOWN",
        "isolated": None,
        "path": str(deploy_dir) if deploy_dir else None,
        "peers": [],
        "shared_count": 0,
        "mutated_checked": 0,
        "examples": [],
        "remedy": None,
        "note": None,
    }
    if not deploy_dir:
        res["note"] = "no deploy dir given"
        return res
    guard = _load_guard(repo_root)
    if guard is None:
        res["note"] = (f"{_GUARD_ERR} -- link group NOT verified. Treat this "
                       "as unknown, not as isolated.")
        return res
    try:
        verdict, facts, _msgs = guard.run_check(str(deploy_dir))
    except Exception as e:  # noqa: BLE001
        res["note"] = f"link check raised {e} -- link group NOT verified"
        return res

    if verdict == "ERROR":
        res["note"] = facts.get("error") or "target unreadable"
        return res

    res["path"] = facts.get("target", str(deploy_dir))
    res["mutated_checked"] = facts.get("mutated_files_checked", 0)
    res["shared_count"] = facts.get("shared_count", 0)
    res["examples"] = [s["rel"] for s in facts.get("shared", [])[:8]]
    res["peers"] = sorted(facts.get("peers", {}).keys())
    if verdict == "SHARED":
        res["verdict"] = "SHARED"
        res["isolated"] = False
        res["remedy"] = (
            f"py -3 scripts/make-isolated-deploy.py --from \"{res['path']}\" "
            f"--to \"{res['path']}-<lane>\"  (links read-only data for free, "
            "copies only the written artifacts)")
    else:
        res["verdict"] = "OK"
        res["isolated"] = True
    return res


def _key(p) -> str:
    try:
        return str(Path(p).resolve()).replace("\\", "/").casefold()
    except Exception:  # noqa: BLE001
        return str(p).replace("\\", "/").casefold()


def cross_lane_link_clash(repo_root: Path, my_deploy_lane: str,
                          other_lanes: dict, my_name: str = "") -> dict:
    """Which OTHER registered lanes are writing the same bytes as this one?

    This is the failure the path-keyed registry is structurally unable to see:
    two lanes name two different directories, the registry reports no
    deploy_lane clash, and both are one install. Resolves each lane's
    deploy_lane to a directory and tests membership in THIS lane's peer set.

    Cheap by construction: the expensive scan runs once, for this lane only;
    every other lane costs one path resolution.
    """
    group = link_group(repo_root, resolve_deploy_dir(my_deploy_lane, repo_root))
    peers = {_key(p) for p in group["peers"]}
    clashes = []
    if peers:
        for name, entry in (other_lanes or {}).items():
            if name == my_name:
                continue
            d = resolve_deploy_dir(entry.get("deploy_lane", ""), repo_root)
            if d and _key(d) in peers:
                clashes.append({
                    "lane": name,
                    "deploy_lane": entry.get("deploy_lane"),
                    "path": str(d),
                    "worktree": entry.get("worktree"),
                })
    return {"link_group": group, "same_bytes_as": clashes}

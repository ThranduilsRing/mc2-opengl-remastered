#!/usr/bin/env python3
"""
MCP-ANTI-CHURN-1: deploy_status.py
One-call deploy-lane health: fingerprint vs a worktree HEAD, manifest
staleness, lane-lock + smoke-lease holders.

Grounded failures (2026-07-01): agents deployed to wrong lanes / smoked stale
exes (DEPLOY_FINGERPRINT mismatch x3); smoke.lock/lease contention blind-waits.

This is a READ-ONLY wrapper around the print-free check functions of
scripts/check-deploy-target.py (DEPLOY-LANE-GUARD-1) — it NEVER writes the
lane lock (that stays the smoking session's job) — plus the smoke lease
registry (scripts/smoke_lib/deploy_lease.py, A:/Games/mc2-opengl/.smoke_leases.json).

Verdict mirrors check-deploy-target.py ordering (most severe wins):
  BROKEN-TREE > CONTENDED > FINGERPRINT-MISMATCH > STALE > NO-EXE > OK
"""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

_CDT_CACHE = None


def _load_cdt(repo_root: Path):
    """Import scripts/check-deploy-target.py (hyphenated name) as a module."""
    global _CDT_CACHE
    if _CDT_CACHE is not None:
        return _CDT_CACHE
    path = Path(repo_root) / "scripts" / "check-deploy-target.py"
    spec = importlib.util.spec_from_file_location("check_deploy_target", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # module body defines functions only; no side effects
    _CDT_CACHE = mod
    return mod


def _load_lease(repo_root: Path):
    scripts = str(Path(repo_root) / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    from smoke_lib import deploy_lease  # type: ignore
    return deploy_lease


def _lane_names(repo_root: Path) -> dict:
    """name -> path, merged from check-deploy-target GOOD_DIRS + deploy_lease."""
    names: dict = {}
    try:
        for name, path in _load_lease(repo_root).DEPLOY_FOLDERS:
            names[name] = path
    except Exception:
        pass
    try:
        for name, path in _load_cdt(repo_root).GOOD_DIRS:
            names.setdefault(name, path)
    except Exception:
        pass
    return names


def _git_head(repo_root: Path) -> Optional[str]:
    try:
        r = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=5,
            stdin=subprocess.DEVNULL,
        )
        return r.stdout.strip() or None
    except Exception:
        return None


def _git_branch(repo_root: Path) -> Optional[str]:
    try:
        r = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5,
            stdin=subprocess.DEVNULL,
        )
        return r.stdout.strip() or None
    except Exception:
        return None


def _mtime_iso(p: Path) -> Optional[str]:
    try:
        return datetime.fromtimestamp(p.stat().st_mtime, tz=timezone.utc) \
            .strftime("%Y-%m-%dT%H:%M:%SZ")
    except OSError:
        return None


def deploy_status(repo_root: Path, lane: str, expected_sha: str = "",
                  exe_name: str = "mc2.exe") -> dict:
    """Deploy-lane health report. `lane` is a canonical name (0.4, 0.4d-rc1,
    0.5-testing, ...) or an absolute deploy dir path."""
    repo_root = Path(repo_root)
    cdt = _load_cdt(repo_root)
    names = _lane_names(repo_root)

    lane = (lane or "").strip()
    if not lane:
        return {"error": "lane required — a name or an absolute path",
                "known_lanes": names}
    target = names.get(lane) or lane
    target_p = Path(target)

    branch = _git_branch(repo_root)
    exp_sha = (expected_sha or _git_head(repo_root) or "").lower()

    result: dict = {
        "lane": lane,
        "path": str(target_p),
        "known_lanes": sorted(names),
        "expected": {"sha": exp_sha[:12] or None, "branch": branch,
                     "worktree": str(repo_root)},
    }

    if not target_p.is_dir():
        result.update({"verdict": "MISSING",
                       "action": "deploy dir does not exist — check the lane "
                                 "name against known_lanes"})
        return result

    # 1. Broken-tree quarantine
    bad = cdt.check_broken_tree(str(target_p))

    # exe presence
    exe = target_p / exe_name
    exe_present = exe.is_file() and exe.stat().st_size > 0
    result["exe"] = {
        "present": exe_present,
        "bytes": exe.stat().st_size if exe.is_file() else 0,
        "mtime_utc": _mtime_iso(exe),
    }

    # 2. Lane lock (per-dir .mc2_deploy_lane.json)
    lock = cdt.read_lane_lock(str(target_p))
    foreign, contended = cdt.check_contention(str(target_p), branch, exp_sha)
    result["lane_lock"] = None
    if lock:
        age = cdt._lock_age_secs(lock)
        result["lane_lock"] = {
            **{k: lock.get(k) for k in ("branch", "sha", "pid", "timestamp",
                                        "intended_test", "host")},
            "age_secs": round(age) if age is not None else None,
            "fresh": cdt.lock_is_fresh(lock),
            "foreign": contended,
        }

    # 3. Smoke lease (shared A:/Games/mc2-opengl/.smoke_leases.json)
    result["smoke_lease"] = None
    try:
        dl = _load_lease(repo_root)
        reg = dl._read_registry()
        entry = reg.get(dl._folder_key(str(target_p)))
        if entry:
            age = dl._age_secs(entry.get("last_used_utc", ""))
            result["smoke_lease"] = {
                **{k: entry.get(k) for k in ("pid", "hostname", "worktree",
                                             "branch", "sha", "intended_test",
                                             "acquired_utc", "last_used_utc")},
                "age_secs": round(age) if age is not None else None,
                "stale": dl._is_stale(entry),
            }
    except Exception as e:
        result["smoke_lease"] = {"error": f"lease registry unreadable: {e}"}

    # 4. Fingerprint (embedded sha/branch scan of the on-disk exe)
    fp_bad, fp_notes = (False, ["exe absent — fingerprint skipped"])
    if exe_present:
        fp_bad, fp_notes = cdt.check_fingerprint(str(exe), exp_sha or None, branch)
        fp = cdt.scan_exe_fingerprint(str(exe))
        result["fingerprint"] = {"embedded_shas": fp["shas"],
                                 "embedded_branches": fp["branches"],
                                 "mismatch": fp_bad, "notes": fp_notes}
    else:
        result["fingerprint"] = {"embedded_shas": [], "embedded_branches": [],
                                 "mismatch": False, "notes": fp_notes}

    # 5. Manifest coherence
    stale, coh_notes = cdt.check_manifest_coherence(str(target_p), exp_sha or None)
    src_commit = cdt.read_manifest_src_commit(str(target_p))
    manifest_csv = target_p / cdt.CSV_MANIFEST_NAME
    result["manifest"] = {
        "src_commit": (src_commit or "")[:12] or None,
        "stale": stale,
        "notes": coh_notes,
        "manifest_mtime_utc": _mtime_iso(manifest_csv),
    }

    # Verdict (most severe wins, mirrors check-deploy-target.py)
    if bad:
        verdict, action = "BROKEN-TREE", bad
    elif contended:
        verdict = "CONTENDED"
        action = (f"dir held by FRESH foreign lane lock (branch="
                  f"{foreign.get('branch')} pid={foreign.get('pid')}) — pick "
                  "another lane from known_lanes or wait")
    elif fp_bad:
        verdict = "FINGERPRINT-MISMATCH"
        action = ("on-disk exe sha != expected — re-deploy with "
                  "scripts/deploy_payload.py before smoking")
    elif stale:
        verdict = "STALE"
        action = ("deploy manifest older than expected HEAD — re-deploy with "
                  "scripts/deploy_payload.py")
    elif not exe_present:
        verdict, action = "NO-EXE", f"{exe_name} absent/empty in {target_p}"
    else:
        verdict, action = "OK", "clear to deploy/smoke"
    result["verdict"] = verdict
    result["action"] = action
    return result


if __name__ == "__main__":  # tiny CLI for ad-hoc use
    root = Path(__file__).resolve().parents[2]
    lane_arg = sys.argv[1] if len(sys.argv) > 1 else "0.4"
    print(json.dumps(deploy_status(root, lane_arg), indent=2))

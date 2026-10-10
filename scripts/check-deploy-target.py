#!/usr/bin/env python3
# scripts/check-deploy-target.py
"""Deploy-target lane guard — pre-smoke self-diagnosis for a deploy folder.

Ends the recurring deploy-dir pain seen across concurrent build lanes:
  1. BROKEN-TREE  — the 0.4c install (stock + modified exe both crash_silent,
                    0 frames) got smoked repeatedly and wasted cycles. Quarantine
                    a known-bad list and refuse, pointing at a good dir.
  2. CONTENDED    — v0.4 / v0.4d-rc1 shared with a concurrent lane that overwrites
                    the exe mid-smoke -> DEPLOY_FINGERPRINT mismatch + false crash.
                    A lightweight per-dir lock file (.mc2_deploy_lane.json) records
                    {sha,branch,pid,timestamp}; a FRESH lock owned by a DIFFERENT
                    branch/sha = the dir is contended -> warn + suggest a free dir.
  3. FINGERPRINT-MISMATCH — after a deploy, the on-disk exe must actually carry
                    the expected sha/branch. We grep the exe (like agents do): the
                    cmake fingerprint header (GenerateBuildFingerprint.cmake) bakes
                    MC2_BUILD_GIT_SHA "<12hex>" and MC2_BUILD_GIT_BRANCH "<branch>"
                    into rodata as bounded C strings -> visible with a raw byte scan.
  4. STALE        — the deploy manifest's src_commit != expected HEAD (a fix built
                    but the dir was never re-deployed).

VERDICTS (one, most-severe wins):  OK | CONTENDED | BROKEN-TREE | STALE |
                                   FINGERPRINT-MISMATCH
Each verdict carries a suggested action. Exit code:
    0  OK
    3  BROKEN-TREE
    4  CONTENDED
    5  STALE
    6  FINGERPRINT-MISMATCH
    2  usage / target-missing error

DEFAULT is a standalone advisory tool agents call BEFORE smoke. run_smoke gains
an OPT-IN --guard flag (default behavior unchanged) that shells this and, unless
--guard-fatal is also passed, treats the verdict as advisory.

Pure stdlib. NO engine change. NO run_smoke verdict-path change by default.

Examples:
  # advisory pre-smoke check + write our lane lock
  py -3 scripts/check-deploy-target.py "A:/Games/mc2-opengl/mc2-win64-v0.4" \
      --expected-sha $(git rev-parse HEAD) --branch claude/foo

  # just ask for a free canonical dir
  py -3 scripts/check-deploy-target.py --suggest-free --branch claude/foo
"""
from __future__ import annotations

import argparse
import csv
import datetime
import json
import os
import re
import sys
from pathlib import Path

LANE_LOCK_NAME = ".mc2_deploy_lane.json"
CSV_MANIFEST_NAME = ".deployed_manifest.csv"   # scripts/deploy_payload.py
JSON_MANIFEST_NAME = ".deploy-manifest.json"   # scripts/write-deploy-manifest.py
PREFIX = "[DEPLOY_GUARD]"

# A lane lock younger than this (seconds) still belongs to a live/recent lane;
# older = presumed abandoned (session crashed / smoke ended). Mirrors the smoke
# lease TTL default so the two coordination systems agree on "fresh".
LANE_LOCK_TTL_SECS = 3600

# ---------------------------------------------------------------------------
# Known-good / known-bad deploy dirs. Kept in lockstep with
# .claude/DEPLOY-DIR-LANE-MAP.md and scripts/smoke_lib/deploy_lease.py.
# ---------------------------------------------------------------------------

# Canonical GOOD deploy dirs, preference order (used for free-dir suggestion).
# The lane names and paths come from the one lane model; 0.4c is the golden, not a deploy target.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "repo_intel"))
import lane_model  # noqa: E402

GOOD_DIRS: list[tuple[str, str]] = [(n, p) for n, p in lane_model.install_paths() if n != "0.4c"]

# KNOWN-BAD trees. basename (casefolded) -> reason. mc2-win64-0.4c is the broken
# tree: stock AND modified exe both crash_silent / PAUSE-loop, 0 frames. Smoking
# it produces false crash reports. DO NOT USE. See DEPLOY-DIR-LANE-MAP.md.
KNOWN_BAD: dict[str, str] = {
    "mc2-win64-0.4c":
        "0.4c is a BROKEN tree (stock + modified exe both crash_silent / "
        "PAUSE-loop, 0 frames). Smoking it yields FALSE crash reports.",
}


def out(msg: str) -> None:
    print(f"{PREFIX} {msg}")


def _now() -> datetime.datetime:
    return datetime.datetime.now(datetime.timezone.utc)


def _iso(dt: datetime.datetime) -> str:
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def _basename(target: str) -> str:
    return os.path.basename(os.path.normpath(target)).casefold()


# ---------------------------------------------------------------------------
# 1. Broken-tree quarantine
# ---------------------------------------------------------------------------

def check_broken_tree(target: str) -> str | None:
    """Return a reason string if target is a known-bad tree, else None."""
    return KNOWN_BAD.get(_basename(target))


def _exe_launchable_sniff(target: str, exe_name: str) -> str | None:
    """Cheap launchability sniff: exe present + nonzero. Returns a problem
    string or None. (We do NOT launch — smoke does that. This just catches an
    empty/absent binary before wasting a smoke run.)"""
    exe = os.path.join(target, exe_name)
    if not os.path.isfile(exe):
        return f"exe not found: {exe}"
    if os.path.getsize(exe) == 0:
        return f"exe is zero bytes: {exe}"
    return None


# ---------------------------------------------------------------------------
# 2. Lane-collision detection (per-dir lock file)
# ---------------------------------------------------------------------------

def _pid_alive(pid: int) -> bool:
    """Liveness of the lock owner. `os.kill(pid, 0)` is NOT a probe on Windows (it is TerminateProcess), and
    the pid here comes from another lane's lock file, so the check goes through child_registry.pid_alive
    (OpenProcess + GetExitCodeProcess), which only reads."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from smoke_lib import child_registry
    return child_registry.pid_alive(pid)


def read_lane_lock(target: str) -> dict | None:
    p = os.path.join(target, LANE_LOCK_NAME)
    try:
        with open(p, encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else None
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None


def _lock_age_secs(lock: dict) -> float | None:
    ts = lock.get("timestamp")
    if not ts:
        return None
    for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%dT%H:%M:%S+00:00"):
        try:
            when = datetime.datetime.strptime(ts, fmt).replace(
                tzinfo=datetime.timezone.utc)
            return (_now() - when).total_seconds()
        except ValueError:
            continue
    return None


def lock_is_fresh(lock: dict) -> bool:
    age = _lock_age_secs(lock)
    if age is None:
        return False
    if age > LANE_LOCK_TTL_SECS:
        return False
    pid = lock.get("pid")
    if isinstance(pid, int) and not _pid_alive(pid):
        return False
    return True


def check_contention(target: str, branch: str | None, expected_sha: str | None
                     ) -> tuple[dict | None, bool]:
    """Return (foreign_lock_or_None, is_contended).

    Contended = a FRESH lane lock owned by a DIFFERENT branch or sha exists.
    Our own (matching branch+sha) lock is never contention.
    """
    lock = read_lane_lock(target)
    if not lock or not lock_is_fresh(lock):
        return None, False
    l_branch = lock.get("branch")
    l_sha = (lock.get("sha") or "").lower()
    exp_sha = (expected_sha or "").lower()
    same_branch = branch is not None and l_branch == branch
    same_sha = exp_sha and l_sha and (
        l_sha.startswith(exp_sha[:min(len(l_sha), len(exp_sha), 12)]) or
        exp_sha.startswith(l_sha[:min(len(l_sha), len(exp_sha), 12)]))
    if same_branch and (same_sha or not exp_sha):
        return None, False   # our own lock (or same branch, sha unknown)
    return lock, True


def write_lane_lock(target: str, branch: str | None, sha: str | None,
                    intended: str | None) -> None:
    p = os.path.join(target, LANE_LOCK_NAME)
    rec = {
        "sha": (sha or "").lower() or None,
        "branch": branch,
        "pid": os.getpid(),
        "timestamp": _iso(_now()),
        "intended_test": intended,
        "host": os.environ.get("COMPUTERNAME") or os.environ.get("HOSTNAME"),
    }
    try:
        with open(p, "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=2)
        out(f"lane lock written: {p} (branch={branch} sha={rec['sha']})")
    except OSError as e:
        out(f"WARNING: could not write lane lock {p}: {e}")


def suggest_free_dir(branch: str | None, expected_sha: str | None,
                     exclude: str | None = None) -> tuple[str, str] | None:
    """Return (name, path) of a canonical GOOD dir that is not known-bad, not
    excluded, and not held by a FRESH foreign lane lock. None if all busy."""
    excl = _basename(exclude) if exclude else None
    for name, path in GOOD_DIRS:
        base = _basename(path)
        if base in KNOWN_BAD or base == excl:
            continue
        if not (Path(path).is_dir() and (Path(path) / "mc2.exe").is_file()):
            continue
        _lock, contended = check_contention(path, branch, expected_sha)
        if contended:
            continue
        return name, path
    return None


# ---------------------------------------------------------------------------
# 3. Fingerprint gate — grep the exe for embedded sha/branch (survives copies)
# ---------------------------------------------------------------------------

# The cmake fingerprint header bakes, as bounded rodata C strings:
#   MC2_BUILD_GIT_SHA    "<12-hex>"
#   MC2_BUILD_GIT_BRANCH "<branch>"
# so a raw byte scan recovers them even when the startup banner (printf %s) is
# not present as a literal. Bounded by \x00 on both sides = isolated C string.
_SHA_RE = re.compile(rb"\x00([0-9a-f]{12})\x00")
_BRANCH_RE = re.compile(rb"\x00(claude/[A-Za-z0-9._/-]{3,80})\x00")


def scan_exe_fingerprint(exe_path: str) -> dict:
    """Return {shas:[...], branches:[...]} embedded in the exe. Best-effort."""
    res = {"shas": [], "branches": []}
    try:
        data = Path(exe_path).read_bytes()
    except OSError:
        return res
    res["shas"] = sorted({m.group(1).decode() for m in _SHA_RE.finditer(data)})
    res["branches"] = sorted({m.group(1).decode()
                              for m in _BRANCH_RE.finditer(data)})
    return res


def check_fingerprint(exe_path: str, expected_sha: str | None,
                      branch: str | None) -> tuple[bool, list[str]]:
    """Return (mismatch, notes). mismatch=True only when we have an expected
    sha AND the exe carries an embedded sha that does not match it."""
    notes: list[str] = []
    if not expected_sha:
        notes.append("no --expected-sha: fingerprint gate skipped")
        return False, notes
    fp = scan_exe_fingerprint(exe_path)
    if not fp["shas"]:
        notes.append("exe carries no embedded fingerprint sha "
                     "(pre-fingerprint build?) — cannot verify")
        return False, notes
    exp = expected_sha.lower()
    for esha in fp["shas"]:
        n = min(len(esha), len(exp))
        if n >= 7 and esha[:n] == exp[:n]:
            notes.append(f"OK: exe sha={esha} matches expected={exp[:12]}")
            if branch and fp["branches"] and branch not in fp["branches"]:
                notes.append(f"note: expected branch {branch} not among "
                             f"embedded {fp['branches']} (sha still matched)")
            return False, notes
    notes.append(f"MISMATCH: exe embedded sha(s)={fp['shas']} "
                 f"expected={exp[:12]} branch(es)={fp['branches']}")
    return True, notes


# ---------------------------------------------------------------------------
# 4. Manifest coherence — deployed src_commit == expected HEAD
# ---------------------------------------------------------------------------

def read_manifest_src_commit(target: str) -> str | None:
    """Return the src_commit recorded by the newest deploy manifest, or None."""
    csv_p = os.path.join(target, CSV_MANIFEST_NAME)
    json_p = os.path.join(target, JSON_MANIFEST_NAME)
    cands = [p for p in (csv_p, json_p) if os.path.isfile(p)]
    if not cands:
        return None
    newest = max(cands, key=lambda p: os.path.getmtime(p))
    try:
        if newest.endswith(".csv"):
            with open(newest, newline="", encoding="utf-8") as f:
                for row in csv.reader(f):
                    if not row or row[0] in ("manifest_version", "relpath"):
                        continue
                    if len(row) >= 4 and row[3]:
                        return row[3]
        else:
            with open(newest, encoding="utf-8") as f:
                d = json.load(f)
            return d.get("src_commit") or d.get("head") or d.get("git_head")
    except (OSError, json.JSONDecodeError, csv.Error):
        return None
    return None


def check_manifest_coherence(target: str, expected_sha: str | None
                             ) -> tuple[bool, list[str]]:
    """Return (stale, notes). stale=True when manifest src_commit != expected."""
    notes: list[str] = []
    if not expected_sha:
        notes.append("no --expected-sha: manifest coherence skipped")
        return False, notes
    src = read_manifest_src_commit(target)
    if not src:
        notes.append("no deploy manifest src_commit recorded — "
                     "target may never have been deployed via deploy_payload.py")
        return False, notes
    exp = expected_sha.lower()
    s = src.lower()
    # DIRTY-BUILD GUARD (2026-07-12): deploy_payload suffixes src_commit with
    # "-dirty" when the exe was built from an uncommitted tree. Strip it for
    # the sha compare but ALWAYS surface it — a dirty deploy that happens to
    # match the expected sha is still unattributable to exact source state.
    dirty = s.endswith("-dirty")
    if dirty:
        s = s[: -len("-dirty")]
        notes.append("DIRTY: manifest records a dirty-tree build "
                     "(src_commit suffix '-dirty') — the deployed exe's exact "
                     "source state is NOT reproducible from the sha alone")
    n = min(len(s), len(exp))
    if n >= 7 and s[:n] == exp[:n]:
        notes.append(f"OK: manifest src_commit={s[:12]} == expected {exp[:12]}"
                     + (" (but DIRTY)" if dirty else ""))
        return dirty, notes
    notes.append(f"STALE: manifest src_commit={s[:12]} != expected {exp[:12]} "
                 "(fix built but this dir was not re-deployed?)")
    return True, notes


# ---------------------------------------------------------------------------
# Verdict assembly
# ---------------------------------------------------------------------------

VERDICT_EXIT = {
    "OK": 0,
    "BROKEN-TREE": 3,
    "CONTENDED": 4,
    "STALE": 5,
    "FINGERPRINT-MISMATCH": 6,
}


def run_guard(target: str, exe_name: str, expected_sha: str | None,
              branch: str | None, intended: str | None, write_lock: bool
              ) -> tuple[str, str]:
    """Return (verdict, action). Most-severe verdict wins. Prints notes."""
    target = os.path.abspath(target)
    if not os.path.isdir(target):
        out(f"ERROR: target dir does not exist: {target}")
        return "ERROR", "create/point at a real deploy dir"

    out(f"target: {target}")
    out(f"branch: {branch}  expected-sha: "
        f"{(expected_sha or '')[:12] or '(none)'}")

    # 1. Broken-tree quarantine (highest priority — refuse outright).
    bad = check_broken_tree(target)
    if bad:
        out(f"BROKEN-TREE: {bad}")
        free = suggest_free_dir(branch, expected_sha, exclude=target)
        if free:
            action = f"use {free[0]} -> {free[1]}"
        else:
            action = ("no free canonical dir suggestion — pick a GOOD dir from "
                      ".claude/DEPLOY-DIR-LANE-MAP.md manually")
        out(f"ACTION: {action}")
        return "BROKEN-TREE", action

    sniff = _exe_launchable_sniff(target, exe_name)
    if sniff:
        out(f"launchability sniff: {sniff}")

    # 2. Contention.
    foreign, contended = check_contention(target, branch, expected_sha)
    if contended:
        f_branch = foreign.get("branch")
        f_sha = foreign.get("sha")
        age = _lock_age_secs(foreign)
        age_s = f"{int(age)}s" if age is not None else "?"
        out(f"CONTENDED: dir held by FRESH foreign lane lock "
            f"(branch={f_branch} sha={f_sha} pid={foreign.get('pid')} "
            f"age={age_s})")
        free = suggest_free_dir(branch, expected_sha, exclude=target)
        if free:
            action = (f"dir contended by {f_branch}; use {free[0]} -> {free[1]} "
                      "(or a private install)")
        else:
            action = (f"dir contended by {f_branch}; all canonical dirs busy — "
                      "wait or use a private install")
        out(f"ACTION: {action}")
        return "CONTENDED", action

    # 3. Fingerprint gate.
    fp_bad, fp_notes = check_fingerprint(
        os.path.join(target, exe_name), expected_sha, branch)
    for n in fp_notes:
        out(f"fingerprint: {n}")

    # 4. Manifest coherence.
    stale, coh_notes = check_manifest_coherence(target, expected_sha)
    for n in coh_notes:
        out(f"manifest: {n}")

    if fp_bad:
        action = ("on-disk exe sha != expected; re-deploy this dir with "
                  "scripts/deploy_payload.py before smoking")
        out(f"ACTION: {action}")
        return "FINGERPRINT-MISMATCH", action
    if stale:
        action = ("deploy manifest older than expected HEAD; re-deploy with "
                  "scripts/deploy_payload.py before smoking")
        out(f"ACTION: {action}")
        return "STALE", action

    # Clear — optionally claim the dir for our lane.
    if write_lock:
        write_lane_lock(target, branch, expected_sha, intended)
    out("OK: dir is a good tree, uncontended, exe fingerprint + manifest coherent")
    return "OK", "clear to deploy/smoke"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target_dir", nargs="?",
                    help="deploy target dir (e.g. A:/Games/mc2-opengl/mc2-win64-v0.4)")
    ap.add_argument("--exe-name", default="mc2.exe",
                    help="exe filename in the target (default mc2.exe)")
    ap.add_argument("--expected-sha", default=None,
                    help="git HEAD the exe/manifest MUST carry (usually "
                    "$(git rev-parse HEAD) of the building worktree)")
    ap.add_argument("--branch", default=None,
                    help="building branch (for lane-lock ownership + contention)")
    ap.add_argument("--intended-test", default=None,
                    help="free-text tag recorded in the lane lock")
    ap.add_argument("--no-write-lock", action="store_true",
                    help="do NOT claim the dir with a lane lock on an OK verdict")
    ap.add_argument("--suggest-free", action="store_true",
                    help="just print a free canonical GOOD dir and exit 0")
    ap.add_argument("--json", action="store_true",
                    help="also emit a machine-readable verdict line as JSON")
    args = ap.parse_args()

    if args.suggest_free:
        free = suggest_free_dir(args.branch, args.expected_sha,
                                exclude=args.target_dir)
        if free:
            out(f"free dir: {free[0]} -> {free[1]}")
            if args.json:
                print(json.dumps({"free_name": free[0], "free_path": free[1]}))
        else:
            out("no free canonical GOOD dir available")
            if args.json:
                print(json.dumps({"free_name": None, "free_path": None}))
        return 0

    if not args.target_dir:
        ap.error("target_dir is required (or use --suggest-free)")

    verdict, action = run_guard(
        args.target_dir, args.exe_name, args.expected_sha, args.branch,
        args.intended_test, write_lock=not args.no_write_lock)

    out(f"VERDICT: {verdict}")
    if args.json:
        print(json.dumps({"verdict": verdict, "action": action,
                          "target": os.path.abspath(args.target_dir)}))
    return VERDICT_EXIT.get(verdict, 2)


if __name__ == "__main__":
    sys.exit(main())

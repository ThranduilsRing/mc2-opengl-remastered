#!/usr/bin/env python3
"""check-deploy-lane-parity.py -- DEPLOY-LANE-PARITY-1 (HARNESS-HYGIENE-3)

WHY THIS EXISTS
---------------
The repo carries TWO independent deploy-lane registries:

  * scripts/smoke_lib/deploy_lease.py  DEPLOY_FOLDERS
        Canonical for LEASING and for `run_smoke.py --deploy <name>`.
        auto_acquire() resolves --deploy against this list ONLY.

  * scripts/check-deploy-target.py     GOOD_DIRS
        Used by the deploy-target validator and by auto-selection scanning.

The MCP `deploy_status` tool (tools/repo_intel/deploy_status.py::_lane_names)
merges BOTH with `setdefault`, so it answers "yes, that lane exists" for the
UNION -- including names that `--deploy` cannot resolve.

That is not theoretical. Measured on 2026-07-29:

  * `0.3` is in GOOD_DIRS but NOT in DEPLOY_FOLDERS. `deploy_status("0.3")`
    reports a known lane; `run_smoke.py --deploy 0.3` raises LeaseError
    ("unknown deploy name") and exits 6. An operator told the lane is fine,
    then hard-stopped by the runner, drops `--deploy` -- and auto-selection
    picks whatever folder is LRU-free. That is how a foreign exe gets smoked.
  * `0.4d-rc1` is in BOTH but with DIFFERENT PATHS, and both paths exist on
    disk with their own mc2.exe. Two tools mean two different trees by the
    same name -- strictly worse than a missing name, because nothing errors.

CONTRACT ENFORCED HERE
----------------------
  1. A lane name present in both registries MUST map to the same path.
  2. A lane name present in only one registry MUST be listed in
     DOCUMENTED_DIVERGENCE below with a reason.
  3. The MCP union must not introduce a name absent from both.

Any other divergence FAILS (exit 1). Deliberately read-only: it never edits a
registry, because retargeting a deploy lane is a user decision, not a
lint autofix.

Usage:
    py -3 scripts/check-deploy-lane-parity.py
    py -3 scripts/check-deploy-lane-parity.py --selftest
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PREFIX = "[deploy-lane-parity]"

# ---------------------------------------------------------------------------
# Divergences that are KNOWN and deliberately tolerated. Every entry needs a
# reason. Adding a row here is a reviewable act -- it is how a real mismatch
# gets acknowledged instead of silently normalised away.
#
# name -> reason
# ---------------------------------------------------------------------------
DOCUMENTED_DIVERGENCE: dict[str, str] = {
    "0.3":
        "GOOD_DIRS-only, INTENTIONALLY NOT LEASABLE. mc2-win64-v0.3 is a "
        "compromised tree (do not deploy over it). It stays a validator-known "
        "path so check-deploy-target can still recognise and refuse it, but it "
        "must NOT be reachable via `run_smoke.py --deploy 0.3`.",

    # --- lease-only lanes (leasable, but the deploy-target validator does not
    # know them). Found by this checker on its first run, 2026-07-29 -- none of
    # these were in the brief; all pre-date this lane.
    "0.4c":
        "lease-only. mc2-win64-v0.4c is in check-deploy-target's KNOWN_BAD map "
        "by basename (broken tree: stock+modified exe both crash_silent), so it "
        "is refused there rather than listed in GOOD_DIRS. Leasable on purpose "
        "for VK lanes that pin --exe explicitly.",
    "0.5.0":
        "lease-only. Release lane under releases/, which is USER-GATED; "
        "check-deploy-target deliberately does not auto-select release trees.",
    "0.5.1":
        "lease-only. Same as 0.5.0 -- user-gated release lane, not an "
        "auto-selection candidate.",
}

# Same-name/different-path conflicts that are known but UNRESOLVED. These are
# reported LOUDLY on every run and still fail unless --allow-unresolved is
# passed, because picking a winner retargets a deploy lane.
UNRESOLVED_PATH_CONFLICTS: dict[str, str] = {
    "0.4d-rc1":
        "deploy_lease points at releases/mc2-win64-v0.4d-rc1; "
        "check-deploy-target points at mc2-win64-v0.4d-rc1. BOTH exist on disk "
        "with their own mc2.exe. Needs a USER ruling on which tree is the real "
        "0.4d-rc1; releases/ is user-gated so no tool may pick unilaterally.",
}


def _load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _norm(p: str) -> str:
    return str(p).replace("\\", "/").rstrip("/").casefold()


def load_registries(root: Path) -> "tuple[dict, dict, dict]":
    """Return (lease_map, good_map, mcp_union_map)."""
    scripts = str(root / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    from smoke_lib import deploy_lease  # type: ignore
    cdt = _load_module(root / "scripts" / "check-deploy-target.py",
                       "check_deploy_target")
    lease = {n: p for n, p in deploy_lease.DEPLOY_FOLDERS}
    good = {n: p for n, p in cdt.GOOD_DIRS}
    union = dict(lease)
    for n, p in good.items():
        union.setdefault(n, p)
    return lease, good, union


def check(lease: dict, good: dict, union: dict,
          allow_unresolved: bool = False) -> "list[str]":
    """Return a list of failure strings; empty means PASS."""
    fails: list[str] = []

    # Rule 1 -- same name, same path.
    for name in sorted(set(lease) & set(good)):
        if _norm(lease[name]) != _norm(good[name]):
            msg = (f"lane '{name}' maps to DIFFERENT paths: "
                   f"deploy_lease={lease[name]!r} vs GOOD_DIRS={good[name]!r}")
            if name in UNRESOLVED_PATH_CONFLICTS:
                msg += f"  [KNOWN-UNRESOLVED: {UNRESOLVED_PATH_CONFLICTS[name]}]"
                if allow_unresolved:
                    print(f"{PREFIX} WARN: {msg}")
                    continue
            fails.append(msg)

    # Rule 2 -- one-sided names must be documented.
    for name in sorted(set(good) - set(lease)):
        if name not in DOCUMENTED_DIVERGENCE:
            fails.append(
                f"lane '{name}' is in check-deploy-target GOOD_DIRS but NOT in "
                f"deploy_lease.DEPLOY_FOLDERS. MCP deploy_status will report it "
                f"as a known lane while `run_smoke.py --deploy {name}` raises "
                f"LeaseError. Add it to DEPLOY_FOLDERS, or document it in "
                f"DOCUMENTED_DIVERGENCE in this script.")
    for name in sorted(set(lease) - set(good)):
        if name not in DOCUMENTED_DIVERGENCE:
            fails.append(
                f"lane '{name}' is leasable via deploy_lease.DEPLOY_FOLDERS but "
                f"unknown to check-deploy-target GOOD_DIRS, so the deploy-target "
                f"validator cannot vet it. Add it to GOOD_DIRS, or document it "
                f"in DOCUMENTED_DIVERGENCE in this script.")

    # Rule 3 -- the MCP union invents nothing.
    for name in sorted(set(union) - set(lease) - set(good)):
        fails.append(f"MCP lane union contains '{name}' present in neither registry")

    return fails


def _selftest() -> int:
    """Positive AND negative controls -- prove the checker actually fires."""
    ok = True

    def case(label, lease, good, union, expect_fail, allow_unresolved=False):
        nonlocal ok
        fails = check(lease, good, union, allow_unresolved)
        got = bool(fails)
        verdict = "ok" if got == expect_fail else "BROKEN"
        if got != expect_fail:
            ok = False
        print(f"{PREFIX} selftest {verdict}: {label} "
              f"(expected_fail={expect_fail} got_fail={got})"
              + (f" :: {fails[0][:110]}" if fails else ""))

    clean_l = {"0.4": "A:/x/v0.4", "0.5.0": "A:/x/v0.5.0"}
    clean_g = dict(clean_l)
    case("clean registries pass", clean_l, clean_g, dict(clean_l), False)

    # NEGATIVE CONTROL 1: the exact 0.4d-rc1 shape -- same name, different tree.
    bad_g = dict(clean_g); bad_g["0.4"] = "A:/x/releases/v0.4"
    case("same-name/different-path FAILS", clean_l, bad_g, dict(clean_l), True)

    # NEGATIVE CONTROL 2: the exact 0.3 shape -- GOOD_DIRS-only, undocumented.
    extra_g = dict(clean_g); extra_g["9.9"] = "A:/x/v9.9"
    union = dict(clean_l); union["9.9"] = "A:/x/v9.9"
    case("GOOD_DIRS-only undocumented lane FAILS", clean_l, extra_g, union, True)

    # POSITIVE CONTROL: documented one-sided lane passes.
    doc_g = dict(clean_g); doc_g["0.3"] = "A:/x/v0.3"
    union3 = dict(clean_l); union3["0.3"] = "A:/x/v0.3"
    case("documented one-sided lane passes", clean_l, doc_g, union3, False)

    # NEGATIVE CONTROL 3: lease-only lane the validator cannot vet.
    lease_only = dict(clean_l); lease_only["8.8"] = "A:/x/v8.8"
    case("lease-only undocumented lane FAILS", lease_only, clean_g,
         dict(lease_only), True)

    # NEGATIVE CONTROL 4: union invents a name.
    inv = dict(clean_l); inv["7.7"] = "A:/x/v7.7"
    case("MCP union inventing a lane FAILS", clean_l, clean_g, inv, True)

    print(f"{PREFIX} selftest {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true",
                    help="run positive/negative controls and exit")
    ap.add_argument("--allow-unresolved", action="store_true",
                    help="downgrade KNOWN-UNRESOLVED path conflicts to warnings")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    lease, good, union = load_registries(ROOT)
    fails = check(lease, good, union, args.allow_unresolved)

    if fails:
        print(f"FAIL: {PREFIX} deploy-lane registries diverge:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        print("", file=sys.stderr)
        print("  Source of truth for LEASING is deploy_lease.DEPLOY_FOLDERS; a "
              "name it cannot resolve is not a usable --deploy target no matter "
              "what MCP deploy_status reports.", file=sys.stderr)
        return 1

    print(f"PASS: {PREFIX} {len(set(lease) | set(good))} lane name(s) coherent "
          f"across deploy_lease ({len(lease)}), GOOD_DIRS ({len(good)}) and the "
          f"MCP union ({len(union)}); "
          f"{len(DOCUMENTED_DIVERGENCE)} documented divergence(s), "
          f"{len(UNRESOLVED_PATH_CONFLICTS)} known-unresolved conflict(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

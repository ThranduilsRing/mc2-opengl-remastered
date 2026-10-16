#!/usr/bin/env python3
"""check-env-drive-guard.py -- ENV-DRIVE-GUARD-1 (HARNESS-HYGIENE-3)

Ratchet for the corrected ENV-DROP guard in scripts/run_smoke.py.

THE DEFECT THIS LOCKS IN A FIX FOR
----------------------------------
The guard used to refuse to start (exit 2) whenever ANY MC2_* var was set in
the shell but absent from run_smoke's env_extra "allowlist", justified by the
claim that unlisted vars are "silently dropped by the Popen env-replacement".

That claim is false. scripts/smoke_lib/runner.py builds the child env as
os.environ.copy() and then .update()s it, so ambient MC2_* vars reach mc2.exe
untouched. The guard was fail-closed and therefore safe, but it hard-stopped
legitimate A/B runs -- an OFF arm setting MC2_TERRAIN_LOD_CHUNK=0 is exactly
the thing an operator needs to be able to do -- while explaining itself with a
mechanism that does not exist.

The guard now distinguishes:
  * UNDRIVEABLE -- runner.FORCED_ENV / POPPED_ENV / a conflicting env_extra
    actually overrides the ambient value. Refuse (fail-closed, unchanged).
  * DELIBERATE  -- the var passes through. Record it in the run manifest and
    proceed.

This checker proves BOTH halves against the real runner tables, so a future
edit that re-broadens the refusal (or narrows it to nothing) fails CI.

Usage:
    py -3 scripts/check-env-drive-guard.py
    py -3 scripts/check-env-drive-guard.py --selftest
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
PREFIX = "[env-drive-guard]"


def _runner():
    from smoke_lib import runner  # type: ignore
    return runner


def run_checks() -> "list[str]":
    r = _runner()
    ua = r.undriveable_ambient
    fails: list[str] = []

    def expect(label, environ, env_extra, want_undriveable):
        got = set(ua(environ, env_extra))
        if got != set(want_undriveable):
            fails.append(f"{label}: expected undriveable={sorted(want_undriveable)} "
                         f"got={sorted(got)}")

    # 1. The premise check. A plain feature gate set in the shell must be
    #    DRIVEABLE -- this is the case the old guard wrongly refused.
    expect("plain gate passes through",
           {"MC2_TERRAIN_LOD_CHUNK": "0"}, {}, [])
    expect("default-ON gate passes through",
           {"MC2_GPU_MECHS": "0"}, {}, [])
    expect("both A/B arms driveable",
           {"MC2_TERRAIN_LOD_CHUNK": "0", "MC2_GPU_MECHS": "0"}, {}, [])

    # 2. Non-MC2 vars are never considered.
    expect("non-MC2 ignored", {"PATH": "x", "TEMP": "y"}, {}, [])

    # 3. FORCED_ENV really is undriveable -- with a CONFLICTING value.
    if not r.FORCED_ENV:
        fails.append("FORCED_ENV is empty; the guard would never fire")
    for name, forced in r.FORCED_ENV.items():
        conflict = "0" if forced != "0" else "1"
        expect(f"FORCED_ENV {name} conflict refused",
               {name: conflict}, {}, [name])
        # ...but agreeing with the forced value is NOT a problem.
        expect(f"FORCED_ENV {name} agreeing is fine", {name: forced}, {}, [])

    # 4. POPPED_ENV is undriveable at any value. An empty table is valid: it lists retired gates whose stale
    #    shell values must still be dropped, and none is needed once the gates are gone (selftest control C
    #    proves the pop path still refuses with a member present).
    for name in r.POPPED_ENV:
        expect(f"POPPED_ENV {name} refused", {name: "1"}, {}, [name])

    # 5. SOFT_ENV is driveable (setdefault -- ambient wins). Regression guard:
    #    if someone moves MC2_FORCE_ALL_VISIBLE into FORCED_ENV, visibility
    #    tests silently stop being able to turn it off.
    for name, soft in r.SOFT_ENV.items():
        other = "0" if soft != "0" else "1"
        expect(f"SOFT_ENV {name} stays driveable", {name: other}, {}, [])

    # 6. env_extra that CONFLICTS with ambient is undriveable; agreeing is not.
    expect("conflicting env_extra refused",
           {"MC2_SMOKE_SEED": "0xBAD"}, {"MC2_SMOKE_SEED": "0xC0FFEE"},
           ["MC2_SMOKE_SEED"])
    expect("agreeing env_extra fine",
           {"MC2_SMOKE_SEED": "0xC0FFEE"}, {"MC2_SMOKE_SEED": "0xC0FFEE"}, [])

    # 7. The two gates named in the HARNESS-HYGIENE-3 brief, explicitly.
    for gate in ("MC2_GPU_MECHS", "MC2_TERRAIN_LOD_CHUNK"):
        if gate in ua({gate: "0"}, {}):
            fails.append(f"{gate} reported undriveable; it passes through "
                         f"os.environ.copy() and must be usable for A/B arms")

    # 8. The false premise must not come back in the guard's own text.
    guard = (ROOT / "scripts" / "run_smoke.py").read_text(encoding="utf-8")
    if "silently dropped by the Popen env-replacement" in guard:
        fails.append("run_smoke.py still states the refuted 'Popen "
                     "env-replacement drops vars' premise")
    if "cannot be DRIVEN by this harness" not in guard:
        fails.append("run_smoke.py lost the corrected UNDRIVEABLE refusal text")
    return fails


def _selftest() -> int:
    """Negative controls: mutate the tables and prove the checker notices."""
    r = _runner()
    ok = True
    saved_forced = dict(r.FORCED_ENV)
    saved_popped = frozenset(r.POPPED_ENV)
    try:
        # Control A: an empty FORCED_ENV must be reported (guard toothless).
        r.FORCED_ENV.clear()
        if not any("FORCED_ENV is empty" in f for f in run_checks()):
            ok = False
            print(f"{PREFIX} selftest BROKEN: empty FORCED_ENV not caught")
        else:
            print(f"{PREFIX} selftest ok: empty FORCED_ENV caught")
        r.FORCED_ENV.update(saved_forced)

        # Control B: moving a SOFT_ENV var into FORCED_ENV must be caught --
        # that is the regression that would silently break visibility tests.
        soft_name = next(iter(r.SOFT_ENV))
        r.FORCED_ENV[soft_name] = r.SOFT_ENV[soft_name]
        if not any(soft_name in f for f in run_checks()):
            ok = False
            print(f"{PREFIX} selftest BROKEN: SOFT->FORCED demotion not caught")
        else:
            print(f"{PREFIX} selftest ok: SOFT->FORCED demotion caught")
        r.FORCED_ENV.pop(soft_name, None)

        # Control C: a member added to POPPED_ENV must be exercised by the pop path and pass (the table may be empty).
        r.POPPED_ENV = frozenset({"MC2_SELFTEST_POPPED"})
        if run_checks():
            ok = False
            print(f"{PREFIX} selftest BROKEN: a POPPED_ENV member is not refused")
        else:
            print(f"{PREFIX} selftest ok: a POPPED_ENV member is refused")
        r.POPPED_ENV = saved_popped

        # Control D: the unmutated tree must PASS (positive control).
        if run_checks():
            ok = False
            print(f"{PREFIX} selftest BROKEN: clean tree does not pass")
        else:
            print(f"{PREFIX} selftest ok: clean tree passes")
    finally:
        r.FORCED_ENV.clear(); r.FORCED_ENV.update(saved_forced)
        r.POPPED_ENV = saved_popped

    print(f"{PREFIX} selftest {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return _selftest()
    fails = run_checks()
    if fails:
        print(f"FAIL: {PREFIX} env-drive guard contract violated:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    r = _runner()
    if not args.quiet:
        print(f"PASS: {PREFIX} {len(r.FORCED_ENV)} forced + {len(r.POPPED_ENV)} "
              f"popped + {len(r.SOFT_ENV)} soft env var(s); ambient MC2_* gates "
              f"stay driveable and are recorded, not refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())

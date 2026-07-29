#!/usr/bin/env python3
# scripts/check-disk-space.py
"""Free-space preflight guard — "do I have room for what I am about to do?"

WHY THIS EXISTS (2026-07-29 ENOSPC incident)
--------------------------------------------
The A: drive reached 0 bytes free while several lanes were working. Because
NO script in this repo checked free space, the failure mode was not a clean
error -- it was SILENT DATA LOSS. A write hit ENOSPC part-way through and
TRUNCATED THREE FILES TO ZERO BYTES. They were recovered only because sibling
worktrees happened to hold copies. Four lanes lost hours of uncommittable work.
Nobody saw a diagnostic; they saw corruption.

`shutil.disk_usage()` is an O(1) syscall. There is no excuse for a lane to
discover ENOSPC by truncating a file.

WHAT MAKES THIS DIFFERENT FROM A FIXED THRESHOLD
-------------------------------------------------
This guard answers a *sized* question, not "is the disk kind of full". Callers
pass the bytes they are about to write (--need-bytes / --need-gb). A release
re-cut needs ~20 GB; a 3rdparty seed needs 1 GB. A single global threshold
would be simultaneously too paranoid for one and useless for the other.
`scripts/deploy_payload.py` passes the REAL enumerated payload size, so the
number is measured, not guessed.

VERDICTS (one, most-severe wins):  OK | LOW | CRITICAL
    OK        free >= need + warn-margin. Proceed.
    LOW       free >= need but < need + warn-margin. Proceed, but the next
              operation of this size will not fit. Surfaced loudly.
    CRITICAL  free < need. The operation CANNOT complete. Do not start it.

Exit codes (distinct, per the check-deploy-target.py convention):
    0  OK
    1  LOW           (advisory: caller decides)
    3  CRITICAL      (hard: caller should abort)
    2  usage / path error

ADVISORY-FIRST, BUT NOT AT THE CALL SITES THAT MATTER
------------------------------------------------------
This script always reports the honest verdict; it never softens CRITICAL. The
*call site* decides what to do with exit 3. The 2026-07-29 incident is the
argument against a purely advisory rollout: a warning that scrolls by is what
we already had (nothing), so the two sites where the money is actually spent --
`scripts/build_release.sh` and `scripts/deploy_payload.py` -- treat CRITICAL as
FATAL from day one. Lighter sites (worktree bootstrap, visual capture,
renderdoc capture) warn and proceed. See docs/disk-space-guard.md.

Pure stdlib. Read-only. No build. Sub-second.

Examples:
  # release re-cut needs 20 GB in release_assets/
  py -3 scripts/check-disk-space.py --path release_assets --need-gb 20

  # deploy: exact enumerated payload size + 15% headroom
  py -3 scripts/check-disk-space.py --path "$TARGET" --need-bytes 4823551104

  # ratchet: prove the guard actually fires
  py -3 scripts/check-disk-space.py --selftest
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PREFIX = "[DISK_GUARD]"

# Default warn margin: how much slack beyond `need` before we stop calling it
# comfortable. 5 GB is roughly one deploy payload, i.e. "there is room for this
# operation AND one more ordinary thing".
DEFAULT_WARN_GB = 5.0

GIB = 1024 ** 3

VERDICT_EXIT = {
    "OK": 0,
    "LOW": 1,
    "CRITICAL": 3,
    "ERROR": 2,
}


def out(msg: str) -> None:
    print(f"{PREFIX} {msg}")


def err(msg: str) -> None:
    print(f"{PREFIX} {msg}", file=sys.stderr)


def gb(n: float) -> str:
    """Format a byte count as binary GB, matching how `du`/this repo speak."""
    return f"{n / GIB:.2f} GB"


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------

def resolve_probe_dir(path: str) -> tuple[Path | None, str]:
    """Return (existing_dir_to_probe, note).

    Callers routinely ask about a directory that does not exist yet -- an
    OUTDIR about to be `mkdir -p`'d, a mktemp staging root, a deploy target
    being created. shutil.disk_usage needs an existing path, so walk UP to the
    nearest existing ancestor. That ancestor is on the same volume, which is
    all the free-space question actually depends on.
    """
    # Reject structurally-invalid paths BEFORE the ancestor walk. Without this
    # the walk happily climbs past a garbage leaf to a real ancestor and
    # returns OK -- i.e. it would launder a bad path into a pass, which is the
    # exact silent-success shape this guard exists to eliminate. (Caught by
    # --selftest; Path.exists() returns False rather than raising for a NUL on
    # some platforms, so an exception check alone is not enough.)
    if not path or "\x00" in path:
        return None, f"invalid path {path!r} (empty or contains NUL)"
    try:
        p = Path(path).expanduser()
    except (OSError, ValueError) as e:
        return None, f"unusable path {path!r}: {e}"

    try:
        p = p.resolve()
    except OSError:
        p = p.absolute()

    original = p
    seen = 0
    while True:
        if p.exists():
            probe = p if p.is_dir() else p.parent
            note = ""
            if probe != original:
                note = (f"path does not exist yet; probing nearest existing "
                        f"ancestor {probe}")
            return probe, note
        parent = p.parent
        if parent == p:
            return None, f"no existing ancestor for {original}"
        p = parent
        seen += 1
        if seen > 64:
            return None, f"path too deep to resolve: {original}"


def volume_label(p: Path) -> str:
    """Human name for the volume, so the failure message NAMES THE DRIVE."""
    drive, _rest = os.path.splitdrive(str(p))
    if drive:
        return drive
    # POSIX: report the mount point.
    cur = p
    try:
        dev = os.stat(cur).st_dev
        while cur.parent != cur and os.stat(cur.parent).st_dev == dev:
            cur = cur.parent
        return str(cur)
    except OSError:
        return str(p)


# ---------------------------------------------------------------------------
# Core check  (pure: no I/O beyond the one disk_usage syscall)
# ---------------------------------------------------------------------------

def classify(free: int, need: int, warn: int) -> str:
    """Return a verdict for (free, need, warn-margin). All in bytes."""
    if need < 0 or warn < 0:
        return "ERROR"
    if free < need:
        return "CRITICAL"
    if free < need + warn:
        return "LOW"
    return "OK"


def run_checks(path: str, need_bytes: int, warn_bytes: int,
               label: str | None = None,
               usage_fn=shutil.disk_usage) -> tuple[str, dict, list[str]]:
    """Core logic, separated from I/O. Returns (verdict, facts, messages).

    `usage_fn` is the INJECTION SEAM and it is not decoration. The first
    attempt at this lane "tested" the guard by writing a 35 GB file to A: --
    which filled the drive, killed three other lanes, and reproduced the very
    incident the guard exists to prevent. Testing a full-disk condition must
    never require a full disk. Pass a stub returning a
    shutil._ntuple_diskusage-alike (any object with .total/.used/.free) and the
    entire CRITICAL path is exercised for zero bytes written. See selftest().
    """
    msgs: list[str] = []
    probe, note = resolve_probe_dir(path)
    if probe is None:
        return "ERROR", {"path": path, "error": note}, [f"FAIL: {note}"]
    if note:
        msgs.append(f"note: {note}")

    try:
        usage = usage_fn(str(probe))
    except OSError as e:
        return ("ERROR", {"path": str(probe), "error": str(e)},
                [f"FAIL: cannot stat volume for {probe}: {e}"])

    vol = volume_label(probe)
    verdict = classify(usage.free, need_bytes, warn_bytes)

    facts = {
        "verdict": verdict,
        "op": label,
        "path": str(probe),
        "volume": vol,
        "free_bytes": usage.free,
        "total_bytes": usage.total,
        "need_bytes": need_bytes,
        "warn_margin_bytes": warn_bytes,
        "shortfall_bytes": max(0, need_bytes - usage.free),
    }

    what = f" for {label}" if label else ""
    if verdict == "CRITICAL":
        short = need_bytes - usage.free
        msgs.append(
            f"FAIL: NOT ENOUGH FREE SPACE{what} on drive {vol}")
        msgs.append(f"FAIL:   path       : {probe}")
        msgs.append(f"FAIL:   free       : {gb(usage.free)} "
                    f"({usage.free} bytes)")
        msgs.append(f"FAIL:   needed     : {gb(need_bytes)} "
                    f"({need_bytes} bytes)")
        msgs.append(f"FAIL:   SHORT BY   : {gb(short)} ({short} bytes)")
        msgs.append("FAIL: DO NOT START THIS OPERATION. A write that runs out "
                    "of space mid-way TRUNCATES the file it was writing "
                    "(2026-07-29: three files lost to zero bytes this way).")
        msgs.append(f"FAIL: WHAT TO DO: free at least {gb(short)} on {vol}, "
                    "then re-run. See .claude/DISK-RECLAIM-PLAN.md for the "
                    "ranked, move-only reclaim steps (archive captures and "
                    "superseded release zips to E: -- do not delete).")
    elif verdict == "LOW":
        msgs.append(f"WARN: free space on {vol} is tight{what}: "
                    f"{gb(usage.free)} free vs {gb(need_bytes)} needed "
                    f"(margin below the {gb(warn_bytes)} comfort band)")
        msgs.append("WARN: this operation should fit, but the next one of this "
                    "size will not. Reclaim soon "
                    "(.claude/DISK-RECLAIM-PLAN.md).")
    else:
        msgs.append(f"PASS: {vol} has {gb(usage.free)} free; "
                    f"{gb(need_bytes)} needed{what} "
                    f"(margin {gb(usage.free - need_bytes)})")

    return verdict, facts, msgs


# ---------------------------------------------------------------------------
# Selftest — the ratchet
# ---------------------------------------------------------------------------
# House rule: "a guard only ever seen passing on good input is not evidence
# that it works." Every case below asserts the guard FIRES when it should, and
# stays quiet when it should not. Registered in scripts/check-contracts.sh as
# `disk_guard_selftest`.

def selftest() -> int:
    failures: list[str] = []

    def check(name: str, got, want) -> None:
        if got != want:
            failures.append(f"{name}: got {got!r}, want {want!r}")

    one_gb = GIB
    # --- classify() truth table -------------------------------------------
    check("classify/comfortable", classify(100 * one_gb, 10 * one_gb,
                                           5 * one_gb), "OK")
    check("classify/exactly-need-plus-warn",
          classify(15 * one_gb, 10 * one_gb, 5 * one_gb), "OK")
    check("classify/inside-warn-band",
          classify(12 * one_gb, 10 * one_gb, 5 * one_gb), "LOW")
    check("classify/exactly-need", classify(10 * one_gb, 10 * one_gb,
                                            5 * one_gb), "LOW")
    check("classify/one-byte-short",
          classify(10 * one_gb - 1, 10 * one_gb, 5 * one_gb), "CRITICAL")
    check("classify/full-disk", classify(0, 1, 0), "CRITICAL")
    check("classify/zero-need-empty-disk", classify(0, 0, 0), "OK")
    check("classify/negative-need", classify(one_gb, -1, 0), "ERROR")

    # --- the guard actually FIRES on a real path with an absurd need -------
    # This is the load-bearing case: no real volume has an exabyte free, so a
    # non-CRITICAL verdict here means the guard is not wired to reality.
    absurd = 1 << 60  # 1 EiB
    v, facts, msgs = run_checks(str(ROOT), absurd, 0, label="selftest")
    check("absurd-need/verdict", v, "CRITICAL")
    check("absurd-need/exit", VERDICT_EXIT[v], 3)
    if facts.get("shortfall_bytes", 0) <= 0:
        failures.append("absurd-need: shortfall_bytes not positive")
    if not any(m.startswith("FAIL:") for m in msgs):
        failures.append("absurd-need: emitted no FAIL: line")
    # The message must NAME THE DRIVE -- a failure that does not say which
    # volume is full sends the reader hunting.
    if not any("drive" in m for m in msgs):
        failures.append("absurd-need: message does not name the drive")
    if not facts.get("volume"):
        failures.append("absurd-need: no volume recorded")

    # --- SIMULATED FULL DISK, zero bytes written ---------------------------
    # The whole point of the usage_fn seam. A predecessor attempt at this lane
    # tried to prove this path by writing a 35 GB file to A:, filling the drive
    # and destroying three other lanes' work -- i.e. it reproduced the incident
    # it was hired to prevent. These cases exercise the identical code path
    # against a stubbed volume and touch nothing.
    class _Usage:
        def __init__(self, total, free):
            self.total, self.free, self.used = total, free, total - free

    def fake(total_gb, free_gb):
        return lambda _p: _Usage(int(total_gb * GIB), int(free_gb * GIB))

    # The literal 2026-07-29 condition: 839 GB volume, 0 bytes free, a deploy
    # asking for 4 GB. Must be CRITICAL, must name the drive, must say STOP.
    vf, ff, mf = run_checks(str(ROOT), 4 * one_gb, 5 * one_gb,
                            label="selftest full disk",
                            usage_fn=fake(839, 0))
    check("fake-full/verdict", vf, "CRITICAL")
    check("fake-full/free-bytes", ff["free_bytes"], 0)
    check("fake-full/shortfall", ff["shortfall_bytes"], 4 * one_gb)
    if not any("DO NOT START" in m for m in mf):
        failures.append("fake-full: no 'DO NOT START' instruction emitted")
    if not any("SHORT BY" in m for m in mf):
        failures.append("fake-full: shortfall not stated in the message")

    # One byte short is still CRITICAL -- no rounding slop at the boundary.
    v1, _f1, _m1 = run_checks(
        str(ROOT), 10 * one_gb, 0,
        usage_fn=lambda _p: _Usage(100 * one_gb, 10 * one_gb - 1))
    check("fake-one-byte-short/verdict", v1, "CRITICAL")

    # Nearly-full but the op fits: LOW, proceed-with-warning, never CRITICAL.
    vl, _fl, ml = run_checks(str(ROOT), 4 * one_gb, 5 * one_gb,
                             usage_fn=fake(839, 6))
    check("fake-tight/verdict", vl, "LOW")
    check("fake-tight/exit", VERDICT_EXIT[vl], 1)
    if not any(m.startswith("WARN:") for m in ml):
        failures.append("fake-tight: emitted no WARN: line")

    # Plenty of room: OK, and no FAIL/WARN noise.
    vo, _fo, mo = run_checks(str(ROOT), 4 * one_gb, 5 * one_gb,
                             usage_fn=fake(839, 400))
    check("fake-roomy/verdict", vo, "OK")
    if any(m.startswith(("FAIL:", "WARN:")) for m in mo):
        failures.append("fake-roomy: emitted a FAIL/WARN line on a healthy disk")

    # A volume query that raises must be ERROR, never a silent pass.
    def boom(_p):
        raise OSError(28, "No space left on device")

    vb, _fb, mb = run_checks(str(ROOT), 0, 0, usage_fn=boom)
    check("usage-raises/verdict", vb, "ERROR")
    check("usage-raises/exit", VERDICT_EXIT[vb], 2)
    if not any(m.startswith("FAIL:") for m in mb):
        failures.append("usage-raises: emitted no FAIL: line")

    # --- and stays quiet on a zero-byte ask --------------------------------
    v0, _f0, _m0 = run_checks(str(ROOT), 0, 0, label="selftest")
    check("zero-need/verdict", v0, "OK")
    check("zero-need/exit", VERDICT_EXIT[v0], 0)

    # --- nonexistent path resolves to an existing ancestor -----------------
    ghost = ROOT / "no-such-dir-disk-guard" / "deeper" / "still-not-here"
    vg, fg, _mg = run_checks(str(ghost), 0, 0, label="selftest")
    check("ghost-path/verdict", vg, "OK")
    if not Path(fg["path"]).exists():
        failures.append(f"ghost-path: probe dir {fg['path']} does not exist")

    # --- a nonexistent path still reports CRITICAL for an absurd need ------
    # (i.e. ancestor-walking must not accidentally turn a real failure into a
    # pass -- that would reintroduce the silent-corruption mode.)
    vg2, _fg2, _mg2 = run_checks(str(ghost), absurd, 0, label="selftest")
    check("ghost-path/absurd-need", vg2, "CRITICAL")

    # --- unresolvable path is an ERROR, not a silent pass ------------------
    ve, _fe, me = run_checks("\x00bad", 0, 0)
    check("bad-path/verdict", ve, "ERROR")
    check("bad-path/exit", VERDICT_EXIT[ve], 2)
    if not any(m.startswith("FAIL:") for m in me):
        failures.append("bad-path: emitted no FAIL: line")

    # --- exit codes are distinct ------------------------------------------
    codes = list(VERDICT_EXIT.values())
    if len(set(codes)) != len(codes):
        failures.append(f"exit codes not distinct: {VERDICT_EXIT}")

    if failures:
        for f in failures:
            err(f"FAIL: selftest {f}")
        err(f"VERDICT: SELFTEST-FAIL ({len(failures)} case(s))")
        return 1
    out("PASS: selftest - guard fires on shortfall, stays quiet on fit, "
        "names the drive, and never silently passes a bad path")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--path", default=str(ROOT),
                    help="path the write will land in (need not exist yet; "
                    "the nearest existing ancestor is probed). Default: repo "
                    "root.")
    ap.add_argument("--need-gb", type=float, default=None,
                    help="bytes about to be written, in binary GB")
    ap.add_argument("--need-bytes", type=int, default=None,
                    help="bytes about to be written, exact (preferred when "
                    "the caller has measured the payload)")
    ap.add_argument("--warn-gb", type=float, default=DEFAULT_WARN_GB,
                    help=f"comfort margin beyond --need before the verdict "
                    f"drops to LOW (default {DEFAULT_WARN_GB} GB)")
    ap.add_argument("--label", default=None,
                    help="what the space is for, echoed in the message "
                    "(e.g. 'release re-cut', 'deploy payload')")
    ap.add_argument("--json", action="store_true",
                    help="also emit a machine-readable facts line as JSON")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress PASS/WARN chatter; failures still print")
    ap.add_argument("--selftest", "--self-test", dest="selftest",
                    action="store_true",
                    help="run the ratchet: prove the guard fires. No disk "
                    "threshold is asserted, so this is stable in CI.")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.need_bytes is not None and args.need_gb is not None:
        err("FAIL: pass --need-bytes or --need-gb, not both")
        return 2
    if args.need_bytes is not None:
        need = args.need_bytes
    elif args.need_gb is not None:
        need = int(args.need_gb * GIB)
    else:
        err("FAIL: --need-bytes or --need-gb is required - this guard answers "
            "'do I have room for THIS operation', not 'is the disk full'")
        return 2
    if need < 0:
        err(f"FAIL: required bytes must be >= 0 (got {need})")
        return 2
    if args.warn_gb < 0:
        err(f"FAIL: --warn-gb must be >= 0 (got {args.warn_gb})")
        return 2

    verdict, facts, msgs = run_checks(
        args.path, need, int(args.warn_gb * GIB), label=args.label)

    for m in msgs:
        # Failures go to stderr so they survive a caller redirecting stdout,
        # and so they cannot quietly scroll past in a build log.
        if m.startswith("FAIL:"):
            err(m)
        elif args.quiet and not m.startswith("WARN:"):
            continue
        else:
            out(m)

    if verdict in ("CRITICAL", "ERROR") or not args.quiet:
        line = f"VERDICT: {verdict}"
        (err if verdict in ("CRITICAL", "ERROR") else out)(
            line if verdict in ("CRITICAL", "ERROR") else f"{line}")

    if args.json:
        print(json.dumps(facts))

    return VERDICT_EXIT.get(verdict, 2)


if __name__ == "__main__":
    sys.exit(main())

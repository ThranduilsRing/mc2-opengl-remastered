#!/usr/bin/env python3
"""check-sim-identity-propagation.py -- SIM-IDENTITY-HASH-1 pipe-liveness.

WHY THIS EXISTS (it caught a real, already-committed bug in its own slice)
=========================================================================
The world-state hash crosses FOUR components before anything grades it:

  engine  code/sim_identity.cpp        -> writes the block
  engine  gos_visual_capture.cpp       -> embeds it in the per-bookmark sidecar
  python  run_visual_capture.py        -> lifts it out of the sidecar into the
                                          capture manifest
  python  run_golden_parity.py         -> carries it into the COMBINED manifest
  python  golden_compare.py            -> the gate reads it and refuses

The first version of this slice populated a local `sim_ids` map in
run_golden_parity.py and never put it in the combined manifest. Every link
looked right in isolation. The gate's own self-test passed (it was fed
hand-built manifests with the block at top level), the field-set contract
passed, the kernel tests passed -- and in the REAL pipeline the block never
arrived, `find_block()` returned None, every comparison came back UNVERIFIED,
and the gate graded everything while appearing installed.

That is the exact failure class this repo keeps paying for: a check that passes
its own self-test while dead. A gate is not "wired" because each end compiles;
it is wired when the payload actually traverses the pipe.

WHAT IT CHECKS
==============
Statically, on source text (no build, no capture, no engine):
  1. the engine collector emits a "sim_identity" key
  2. gos_visual_capture.cpp calls mc2SimIdentityWriteJson and splices the result
  3. run_visual_capture.py lifts `sim_identity` out of the sidecar AND puts it
     on each bookmark result
  4. run_golden_parity.py puts a `sim_identity` key in the COMBINED manifest
     dict -- the specific link that was missing
  5. golden_compare.py actually invokes the gate before grading

WHAT IT DOES NOT CHECK
======================
That the bytes are well-formed at runtime. No capture has ever run in this lane
(engine launches are blocked), so the JSON emitter is compile-verified and
schema-verified only. A malformed-but-present block would satisfy every rule
here and still land as UNVERIFIED. That gap is real and is named in the slice's
status notes rather than papered over.

Exit 0 = every link present. 1 = a link is missing (named). 2 = a file is
missing, which is never a pass.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (label, path, [required substrings], why it matters)
LINKS = [
    ("engine collector emits the block",
     "code/sim_identity.cpp",
     ['\\"sim_identity\\": {'],
     "without this there is no payload at all"),

    ("capture sidecar splices the block in",
     "GameOS/gameos/gos_visual_capture.cpp",
     ["mc2SimIdentityWriteJson", "sib.data()"],
     "the block would never reach a file on disk"),

    ("run_visual_capture lifts it out of the sidecar",
     "scripts/run_visual_capture.py",
     ['sc.get("sim_identity")', '"sim_identity": simid'],
     "the block would die at the sidecar and never reach a manifest"),

    ("run_golden_parity puts it in the COMBINED manifest",
     "scripts/run_golden_parity.py",
     ['"sim_identity": _first_sim_identity(sim_ids)'],
     "THE LINK THAT WAS MISSING: the gate reads the combined manifest, so "
     "without this key every comparison is UNVERIFIED and grades silently"),

    ("golden_compare runs the gate before grading",
     "scripts/golden_compare.py",
     ["_sim_identity_gate(", "_SIM_EXIT_UNGRADED"],
     "the verdict would be computed by nobody"),
]


def main() -> int:
    failures = []
    missing_files = []
    for label, rel, needles, why in LINKS:
        p = ROOT / rel
        if not p.exists():
            missing_files.append(rel)
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        absent = [n for n in needles if n not in text]
        if absent:
            failures.append((label, rel, absent, why))

    if missing_files:
        for rel in missing_files:
            print("[sim-identity-propagation] MISSING FILE: %s" % rel)
        print("[sim-identity-propagation] RESULT: FAIL (files missing -- the "
              "chain was not checked, which is NOT a pass)")
        return 2

    if failures:
        for label, rel, absent, why in failures:
            print("[sim-identity-propagation] BROKEN LINK: %s" % label)
            print("    file    : %s" % rel)
            print("    missing : %s" % ", ".join(repr(a) for a in absent))
            print("    why     : %s" % why)
        print("[sim-identity-propagation] RESULT: FAIL (%d broken link(s)) -- "
              "the refuse-to-grade gate is INSTALLED BUT DEAD; comparisons will "
              "return UNVERIFIED and grade anyway." % len(failures))
        return 1

    print("[sim-identity-propagation] all %d links present: engine -> sidecar "
          "-> capture manifest -> combined manifest -> gate" % len(LINKS))
    print("[sim-identity-propagation] RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

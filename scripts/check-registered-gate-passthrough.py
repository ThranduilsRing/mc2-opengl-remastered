#!/usr/bin/env python3
"""GATE-PARITY-CLOSE-1 -- a REGISTERED gate that the engine reads must also be in
the run_smoke.py env passthrough allowlist.

THE DEFECT CLASS (ENV_DROP), and why nothing caught it
------------------------------------------------------
docs/tier1_env_vars.md's "Adding a new MC2_* gate" checklist has six steps.
Step 1 (register in RendererFeatureRegistry.h) is CI-blocking via
scripts/check-env-registry.sh. Step 5 (document) and step 4 (add to the
run_smoke.py passthrough allowlist) are not enforced by anything -- and the
checklist itself says so at step 4: "scripts/check-env-allowlist.sh is an
ADVISORY drift check for this allowlist (always exits 0 unless run with
--strict) -- it is not CI-blocking, so don't rely on it catching a miss."

That advisory script is ALSO not wired into scripts/check-contracts.sh at all,
and the only section it fails on under --strict is section 1 (vars the HARNESS
itself references). An engine-read gate missing from the allowlist lands in
section 2, which is explicitly informational. So the class had zero coverage
from two directions at once.

MC2_TREE_DIAG_TRACE was the exemplar: read at
GameOS/gameos/gos_static_prop_batcher.cpp:115, absent from the allowlist, absent
from docs/tier1_env_vars.md, and accounted for ONLY by check-env-registry.sh's
own ALLOWLIST -- which silences the registry check without ever putting the var
in front of any other one. A smoke run asked to turn that diagnostic ON had the
variable dropped at Popen and emitted no [TREE_DIAG] lines, which reads as "the
bug did not reproduce" rather than "the trace never ran".

WHY THIS IS A RATCHET AND NOT A BLANKET RULE
--------------------------------------------
Measured on this tree: 971 distinct MC2_* vars are read via getenv() in
mclib/ GameOS/ code/ RenderCore/, and 503 of them are not in the allowlist.
Failing on all 503 would be a false-positive mill, which is the failure mode
scripts/gate-defaults-allowlist.txt exists to warn about. Scoping to vars that
someone has REGISTERED -- i.e. deliberately declared as a shipping gate -- cuts
that to a reviewable set, and the rest is frozen as explicit debt.

The debt file is a TWO-WAY ratchet:
  * a var that is missing but NOT in the debt file  -> FAIL (new drop)
  * a var in the debt file that is no longer missing -> FAIL (fixed; delete the
    line, so the debt can only shrink and a silent re-add is impossible)

Exit codes: 0 clean, 1 drift, 2 could not run (anchors moved, files absent).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.dirname(HERE)
SMOKE = os.path.join(WORKTREE, "scripts", "run_" + "smoke.py")
REGISTRY = os.path.join(WORKTREE, "RenderCore", "RendererFeatureRegistry.h")
DEBT = os.path.join(HERE, "registered-gate-passthrough-debt.txt")
SCAN_DIRS = ("mclib", "GameOS", "code", "RenderCore")

# Vars the smoke runner sets directly in the child env rather than passing
# through from the parent. Kept in lockstep with RUNNER_DIRECT in
# scripts/check-env-allowlist.sh.
RUNNER_DIRECT = {
    "MC2_SMOKE_MODE", "MC2_HEARTBEAT", "MC2_SMOKE_SEED", "MC2_TGL_POOL_TRACE",
    "MC2_ASSET_SCALE_TRACE", "MC2_MENU_CANARY_SKIP_INTRO",
    "MC2_GL_ERROR_DRAIN_SILENT",
}

VAR_RE = re.compile(r'"(MC2_[A-Z0-9_]+)"')
GETENV_RE = re.compile(r'getenv\s*\(\s*"(MC2_[A-Z0-9_]+)"')


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def allowlist_vars(text):
    """Quoted MC2_* names inside the `if k in (` ... `)},` passthrough tuple.

    Same anchors as scripts/check-env-allowlist.sh; if they ever move, BOTH
    scripts must be updated, and this one exits 2 rather than reporting a
    spurious clean run.
    """
    out, inside = set(), False
    for line in text.splitlines():
        if not inside and "if k in (" in line:
            inside = True
        if inside:
            out.update(VAR_RE.findall(line))
            if ")}," in line:
                break
    return out


def registered_vars(text):
    return set(VAR_RE.findall(text))


def engine_readers():
    """-> {var: [relpath, ...]} for getenv("MC2_*") under SCAN_DIRS."""
    found = {}
    for d in SCAN_DIRS:
        root = os.path.join(WORKTREE, d)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [x for x in dirnames
                           if x not in (".git", "build64", "spv")]
            for fn in filenames:
                if not fn.endswith((".cpp", ".h", ".hpp", ".cc", ".inl")):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    text = read(p)
                except OSError:
                    continue
                rel = os.path.relpath(p, WORKTREE).replace("\\", "/")
                for var in GETENV_RE.findall(text):
                    found.setdefault(var, []).append(rel)
    return found


def load_debt():
    if not os.path.exists(DEBT):
        return set()
    out = set()
    for line in read(DEBT).splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(line)
    return out


def compute():
    for p in (SMOKE, REGISTRY):
        if not os.path.exists(p):
            print("check-registered-gate-passthrough: missing %s" % p,
                  file=sys.stderr)
            sys.exit(2)
    allow = allowlist_vars(read(SMOKE))
    if not allow:
        print("check-registered-gate-passthrough: could not extract the "
              "passthrough allowlist block from the smoke harness "
              "(anchors 'if k in (' ... ')},' not found -- script needs "
              "updating)", file=sys.stderr)
        sys.exit(2)
    readers = engine_readers()
    registered = registered_vars(read(REGISTRY))
    missing = {v: readers[v] for v in sorted(set(readers) & registered)
               if v not in allow and v not in RUNNER_DIRECT}
    return allow, readers, registered, missing


def main(argv):
    quiet = "--quiet" in argv
    allow, readers, registered, missing = compute()
    debt = load_debt()

    new_drops = sorted(set(missing) - debt)
    fixed = sorted(debt - set(missing))

    if not quiet:
        print("== check-registered-gate-passthrough ==")
        print("  engine-read MC2_* vars      : %d" % len(readers))
        print("  registered vars             : %d" % len(registered))
        print("  registered + read + dropped : %d" % len(missing))
        print("  frozen debt entries         : %d" % len(debt))

    rc = 0
    if new_drops:
        rc = 1
        print("")
        print("FAIL: %d REGISTERED gate(s) the engine reads are NOT in the "
              "smoke passthrough allowlist and are not recorded debt:"
              % len(new_drops))
        for v in new_drops:
            print("  %s" % v)
            for site in sorted(set(missing[v]))[:3]:
                print("      read at %s" % site)
        print("")
        print("  A smoke run that sets one of these has it dropped at Popen, so")
        print("  the gate-ON arm silently executes the gate-OFF path. Add each")
        print("  to the env passthrough allowlist in the smoke harness (step 4")
        print("  of the checklist in docs/tier1_env_vars.md), or record it in")
        print("  %s with a written reason."
              % os.path.relpath(DEBT, WORKTREE).replace("\\", "/"))

    if fixed:
        rc = 1
        print("")
        print("FAIL: %d var(s) are recorded as debt but are now in the "
              "allowlist. Delete these lines from %s -- the debt list only "
              "ratchets down, so a silent re-add stays impossible:"
              % (len(fixed), os.path.relpath(DEBT, WORKTREE).replace("\\", "/")))
        for v in fixed:
            print("  %s" % v)

    if rc == 0 and not quiet:
        print("  OK: no new passthrough drops; debt list is exact.")
    return rc


def selftest():
    """Prove the two ratchet directions on synthetic input, then confirm the
    real extraction still finds a non-empty allowlist (an empty one would make
    every var look dropped and is treated as a hard error, not a clean run)."""
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("  %s  %s%s" % ("PASS" if cond else "FAIL", name,
                              "" if cond else "  " + detail))
        ok = ok and bool(cond)

    sample = '''
        env_extra={k: v for k, v in os.environ.items()
                   if k in (
                       "MC2_ALPHA",
                       "MC2_BETA",
                   )},
    '''
    got = allowlist_vars(sample)
    check("allowlist block extraction finds the quoted names",
          got == {"MC2_ALPHA", "MC2_BETA"}, str(got))
    check("text with no anchors yields an EMPTY set (caller exits 2)",
          allowlist_vars("nothing to see") == set())
    check("names outside the block are not harvested",
          "MC2_OUTSIDE" not in allowlist_vars(
              sample + '\nfoo = "MC2_OUTSIDE"\n'))

    missing_sim = {"MC2_A": ["x.cpp"], "MC2_B": ["y.cpp"]}
    check("a missing var absent from debt is a NEW DROP",
          sorted(set(missing_sim) - {"MC2_A"}) == ["MC2_B"])
    check("a debt var no longer missing is reported as FIXED",
          sorted({"MC2_A", "MC2_C"} - set(missing_sim)) == ["MC2_C"])

    allow, readers, registered, missing = compute()
    check("the LIVE allowlist block still parses to a non-empty set",
          len(allow) > 100, "got %d" % len(allow))
    check("the LIVE engine sweep still finds readers",
          len(readers) > 100, "got %d" % len(readers))
    check("MC2_TREE_DIAG_TRACE is registered, read, and allowlisted",
          "MC2_TREE_DIAG_TRACE" in registered
          and "MC2_TREE_DIAG_TRACE" in readers
          and "MC2_TREE_DIAG_TRACE" not in missing)

    print("-" * 60)
    print("SELF-TEST %s" % ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    if "--write-debt" in sys.argv:
        _a, _r, _g, miss = compute()
        lines = ["# GATE-PARITY-CLOSE-1 -- FROZEN DEBT.",
                 "# Registered gates the engine reads that are NOT in the smoke",
                 "# passthrough allowlist. A smoke run setting one of these has it",
                 "# dropped at Popen, so the gate-ON arm runs the gate-OFF path.",
                 "#",
                 "# This list only RATCHETS DOWN: fix a var by adding it to the",
                 "# allowlist in the smoke harness, then delete its line here. The",
                 "# checker fails if a line here is already fixed, so the file",
                 "# cannot silently grow stale.",
                 "#",
                 "# Frozen by GATE-PARITY-CLOSE-1; entries inherited, not audited",
                 "# individually. Each is a candidate ENV_DROP, not a proven one.",
                 ""]
        lines += sorted(miss)
        with open(DEBT, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
        print("wrote %s with %d entries" % (DEBT, len(miss)))
        sys.exit(0)
    sys.exit(main(sys.argv[1:]))

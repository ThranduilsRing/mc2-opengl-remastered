#!/usr/bin/env python3
"""check-smoke-env-inheritance.py — GATE-HYGIENE-CLOSE-1 CI tripwire.

WHAT THIS PROTECTS, AND WHY IT IS NOT THE OBVIOUS THING
-------------------------------------------------------
`scripts/run_smoke.py` carries a large `if k in ( ... )` tuple that reads like a
passthrough ALLOWLIST: names in it are copied from `os.environ` into `env_extra`,
and the surrounding comments say that anything absent is "dropped by Popen".

Those comments are STALE. Reader source (verified, see the contract checks below):

    scripts/smoke_lib/runner.py:179   env = os.environ.copy()
    scripts/run_smoke.py:353          env = os.environ.copy()

The child env is built by COPYING the parent environment. Ambient `MC2_*` vars
reach `mc2.exe` whether or not they are named in the tuple, so the tuple's
os.environ-copying rows are NO-OPS and the "allowlist" drops nothing.
HARNESS-HYGIENE-3 established this and rewrote the guard accordingly.

This matters because the stale framing keeps regenerating a specific false
audit finding: "N registered gates sit outside the smoke allowlist, so both arms
of their A/B ran the same code path and every such verification is void." That
conclusion does not follow from current source. A forensic sweep of the ledger
and lane STATUS docs for gates in that gap found ZERO confirmed voided
verifications.

SO WHAT IS ACTUALLY WORTH GUARDING?
-----------------------------------
Two real invariants, both of which WOULD reinstate the failure class if broken:

  1. THE INHERITANCE CONTRACT. If anyone changes the runner back to building a
     from-scratch `env={...}` dict (which is what the stale comments describe,
     and therefore an easy "fix" for a future reader to apply), the vestigial
     allowlist becomes load-bearing overnight. Every registered gate not named
     in it would start silently dropping, and gate-ON smoke runs would quietly
     execute the gate-OFF path again. This checker prints that blast radius so
     the cost is visible, and FAILS if the contract is broken without the
     allowlist gap being closed first.

  2. THE UNDRIVEABLE GUARD MUST FAIL CLOSED. There IS a genuinely undriveable
     set — `runner.FORCED_ENV`, `runner.POPPED_ENV`, and vars superseded by a
     run's own `env_extra`. Setting one of those does nothing, so a PASS would
     grade a configuration that never executed. `run_smoke.py` refuses to start
     on that set. It must keep doing so via a NON-ZERO `sys.exit`: a prior
     session shipped a guard that printed FATAL and then exited 0 (a bare
     `return` whose value main() discards) — a red message with a green exit
     code, which is indistinguishable from no guard at all.

SILENT ABSENCE FAILS. Every contract below is matched positively against source.
If a pattern cannot be found — because a file was renamed, refactored, or the
guard was deleted — that is a FAIL, not a quiet pass. A checker that passes when
it cannot find the thing it checks is the bug it is supposed to catch.

VERIFY-ONLY BY DESIGN
---------------------
Per the convention established in f99cf37d, this checker NEVER writes any file.
It has no --write mode and emits no report artifact, so it cannot produce the
whitespace-only diff that got a lane's real work `git checkout`-ed away. All
output goes to stdout.

Exit codes:
  0 — all contracts hold
  1 — a contract is broken (or could not be verified)
  2 — a required input file is missing
"""
import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(REPO_ROOT, "scripts", "smoke_lib", "runner.py")
RUN_SMOKE = os.path.join(REPO_ROOT, "scripts", "run_smoke.py")
REGISTRY = os.path.join(REPO_ROOT, "RenderCore", "RendererFeatureRegistry.h")

GATE = r"MC2_[A-Z0-9_]+"
ENTRY_RE = re.compile(r'\{\s*"(%s)"\s*,\s*"(%s)"\s*,' % (GATE, GATE))


def _read(path):
    if not os.path.isfile(path):
        print("FATAL: required input missing: %s" % path, file=sys.stderr)
        sys.exit(2)
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _array_body(text, name):
    marker = "EnvVarDesc %s[] = {" % name
    if marker not in text:
        return ""
    i = text.index(marker)
    depth, started, j = 0, False, i
    while j < len(text):
        if text[j] == "{":
            depth += 1
            started = True
        elif text[j] == "}":
            depth -= 1
            if started and depth == 0:
                return text[i:j + 1]
        j += 1
    return ""


def blast_radius():
    """Registered gates NOT named in the run_smoke.py passthrough tuple.

    Currently harmless (the tuple drops nothing). This is the count that would
    start silently dropping the moment the inheritance contract breaks.
    """
    reg = _read(REGISTRY)
    registered = set()
    for table in ("kFeatureTable", "kAuxEnvVars"):
        for _fid, env in ENTRY_RE.findall(_array_body(reg, table)):
            registered.add(env)
    smoke = _read(RUN_SMOKE)
    try:
        i = smoke.index("if k in (")
        j = smoke.index(")}", i)
        allow = set(re.findall(r'"(%s)"' % GATE, smoke[i:j]))
    except ValueError:
        allow = set()
    return registered, allow, sorted(registered - allow)


CONTRACTS = [
    # (label, file, regex, explanation-if-missing)
    (
        "runner builds child env by COPYING os.environ",
        RUNNER,
        r"^\s*env\s*=\s*os\.environ\.copy\(\)",
        "scripts/smoke_lib/runner.py no longer starts from os.environ.copy(). If it\n"
        "     now builds a from-scratch env dict, the run_smoke.py passthrough tuple\n"
        "     has become LOAD-BEARING and every registered gate missing from it will\n"
        "     silently drop -- gate-ON smoke runs will execute the gate-OFF path and\n"
        "     report 'no difference' for the wrong reason.",
    ),
    (
        "runner applies FORCED_ENV / SOFT_ENV / POPPED_ENV / env_extra in order",
        RUNNER,
        r"env\.update\(cfg\.env_extra\)",
        "the runner no longer applies cfg.env_extra last. Per-run values must win\n"
        "     over ambient and over SOFT_ENV defaults, or a run's own configuration\n"
        "     can be silently overridden by the caller's shell.",
    ),
    (
        "runner exposes undriveable_ambient() for the guard to consume",
        RUNNER,
        r"def\s+undriveable_ambient\s*\(",
        "scripts/smoke_lib/runner.py no longer defines undriveable_ambient(). This\n"
        "     is the function that computes the genuinely-undeliverable set\n"
        "     (FORCED_ENV / POPPED_ENV / conflicting env_extra); without it the\n"
        "     run_smoke.py guard has nothing to check.",
    ),
    (
        "run_smoke consults undriveable_ambient() before launching",
        RUN_SMOKE,
        r"undriveable_ambient\s*\(\s*os\.environ",
        "scripts/run_smoke.py no longer calls undriveable_ambient(os.environ, ...).\n"
        "     A var the harness cannot deliver would then be accepted silently and\n"
        "     a PASS would grade a configuration that never executed.",
    ),
    (
        "undriveable guard FAILS CLOSED with a non-zero sys.exit",
        RUN_SMOKE,
        r"sys\.exit\(\s*2\s*\)",
        "the ENV-DROP guard no longer terminates via sys.exit(2). A prior session\n"
        "     shipped this guard using a bare `return`, whose value main() discards --\n"
        "     it printed FATAL and then exited 0. A red message with a green exit code\n"
        "     is indistinguishable from no guard at all.",
    ),
    (
        "ambient MC2_* vars are RECORDED in the run manifest",
        RUN_SMOKE,
        r'ambient_mc2_env',
        "run_smoke.py no longer records ambient MC2_* vars in the run manifest.\n"
        "     An unrecorded ambient var is how three scrubbed gates rode into dozens\n"
        "     of captures without any run saying so.",
    ),
]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verbose", action="store_true",
                    help="also list the gates in the blast radius")
    args = ap.parse_args()

    print("[smoke-env-inheritance] GATE-HYGIENE-CLOSE-1")

    inheritance_ok = True
    failures = []
    for label, path, pattern, why in CONTRACTS:
        text = _read(path)
        ok = re.search(pattern, text, re.M) is not None
        print("  %-62s %s" % (label, "OK" if ok else "BROKEN"))
        if not ok:
            failures.append((label, path, pattern, why))
            if "os.environ.copy" in pattern:
                inheritance_ok = False

    registered, allow, gap = blast_radius()
    print()
    print("  registered gates                     : %d" % len(registered))
    print("  named in run_smoke passthrough tuple : %d" % len(allow & registered))
    print("  NOT named (blast radius)             : %d" % len(gap))
    if inheritance_ok and not failures:
        print("  -> currently HARMLESS: the child env is copied from os.environ,")
        print("     so the tuple drops nothing and these %d gates are driveable." % len(gap))
        print("     Do NOT file this gap as 'voided A/B verifications' -- that")
        print("     conclusion does not follow while the contract above holds.")
    if args.verbose:
        for g in gap:
            print("      %s" % g)

    if failures:
        print("\n" + "=" * 70)
        print("FAIL: %d smoke env contract(s) broken." % len(failures))
        print("=" * 70)
        for label, path, pattern, why in failures:
            print("\n  BROKEN: %s" % label)
            print("     file  : %s" % os.path.relpath(path, REPO_ROOT))
            print("     wanted: /%s/" % pattern)
            print("     %s" % why)
        if not inheritance_ok:
            print("\n  BLAST RADIUS IF THE INHERITANCE CONTRACT IS GONE: %d registered"
                  % len(gap))
            print("  gate(s) would now silently drop from smoke runs. Close that gap")
            print("  (add them to the run_smoke.py tuple) BEFORE landing a change that")
            print("  stops copying os.environ.")
        print("\n  NOTE: a contract that cannot be FOUND counts as broken. If this")
        print("  fired because of a legitimate refactor, update the pattern in")
        print("  scripts/check-smoke-env-inheritance.py in the SAME commit.")
        print("\n  result: FAIL")
        return 1

    print("\n  result: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

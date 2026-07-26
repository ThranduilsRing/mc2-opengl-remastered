#!/usr/bin/env python3
"""SMOKE-DEPLOY-TRAP-1 contract: run_smoke.py must never print PASS alongside
a DEPLOY_FINGERPRINT mismatch.

WHY THIS EXISTS
---------------
The single most expensive process bug of the Truth-First arc was `run_smoke.py`
printing `PASS (5/5)` while `DEPLOY_FINGERPRINT` reported MISMATCH -- the runner
had smoked some other lane's stale exe. It bit eight separate lanes; several
reported verification results that were meaningless and had to be redone.

DEPLOY-FINGERPRINT-DEFAULT-ON-1 (e01e8ac1) made the mismatch fatal by default.
This checker exists so that fix cannot silently rot back. It fails if ANY of the
paths that could re-open the trap is reintroduced:

  fp_default_on      MC2_SMOKE_REQUIRE_FINGERPRINT defaults to ON (opt-OUT only)
  fp_clears_verdict  a hard fingerprint failure clears the PASS verdict
  fp_exc_is_failure  a raising fingerprint check counts as a failure, not a skip
  fp_import_fatal    a broken fingerprint import aborts instead of skipping
  fp_docs_honest     no source comment/docstring claims "advisory by default"
  fp_behaviour       the compare function really returns hard_fail for
                     mismatch / absent, and not for a match

Every static check is proven to FIRE by `--selftest`, which mutates a copy of
the source back to the pre-fix shape and asserts the check reports FAIL. A guard
that has only ever been seen passing on good input is not evidence.

Usage:
  py -3 scripts/check-smoke-fingerprint-honesty.py            # contract mode
  py -3 scripts/check-smoke-fingerprint-honesty.py --selftest # prove it fires
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN_SMOKE = ROOT / "scripts" / "run_smoke.py"
FP_LIB = ROOT / "scripts" / "smoke_lib" / "fingerprint.py"

# A source comment that contradicts the code is the same class of defect as the
# bug itself: a lane that reads "advisory by default" concludes its green run
# was ungated, or that it must opt in before its result means anything.
#
# The rule is deliberately NARROW. run_smoke.py hosts several checks that are
# genuinely advisory by default (MC2_SMOKE_REQUIRE_FRESH, _REQUIRE_VISUAL,
# _REQUIRE_DIAG_STATE), and some of their comments legitimately mention
# DEPLOY_FINGERPRINT as the pattern they mirror. A proximity- or phrase-based
# ban flags those correct comments, which would pressure a future lane into
# rewriting accurate documentation to appease a checker. So:
#
#   Rule A: the opt-IN framing "MC2_SMOKE_REQUIRE_FINGERPRINT=1" must not
#           appear. The knob is opt-OUT; only "=0" is a truthful reference.
#   Rule B: a comment paragraph naming MC2_SMOKE_REQUIRE_FINGERPRINT must not
#           call the check advisory.
#
# Both stale comments this arc removed were caught by Rule A.
OPT_IN_FRAMING = re.compile(r"MC2_SMOKE_REQUIRE_FINGERPRINT\s*=\s*[\"']?1")
ADVISORY_WORD = re.compile(r"advisory", re.I)
FP_KNOB = "MC2_SMOKE_REQUIRE_FINGERPRINT"


def _check_default_on(smoke_src: str) -> str | None:
    """MC2_SMOKE_REQUIRE_FINGERPRINT must be opt-OUT, not opt-IN."""
    m = re.search(
        r"_fp_require\s*=\s*os\.environ\.get\(\s*[\"']MC2_SMOKE_REQUIRE_FINGERPRINT[\"']\s*,"
        r"\s*[\"'](?P<default>[^\"']*)[\"']\s*\)\s*(?P<op>!=|==)\s*[\"'](?P<cmp>[^\"']*)[\"']",
        smoke_src)
    if not m:
        return ("could not find the _fp_require assignment in run_smoke.py -- "
                "the fingerprint default-on wiring is gone or was rewritten")
    default, op, cmp_ = m.group("default"), m.group("op"), m.group("cmp")
    # Truth table: the expression must evaluate True when the var is UNSET.
    unset_result = (default != cmp_) if op == "!=" else (default == cmp_)
    if not unset_result:
        return (f"MC2_SMOKE_REQUIRE_FINGERPRINT is opt-IN: with the var unset, "
                f"_fp_require evaluates False (default={default!r} {op} {cmp_!r}). "
                f"A fingerprint mismatch would not clear the PASS verdict.")
    return None


def _check_clears_verdict(smoke_src: str) -> str | None:
    """The hard-fail site must actually set passed = False."""
    m = re.search(r"if\s+_fp_require\s+and\s+_fp_hard_fail\s*:(?P<body>.*?)\n(?=\s{0,4}\S)",
                  smoke_src, re.S)
    if not m:
        return ("no `if _fp_require and _fp_hard_fail:` verdict site in "
                "run_smoke.py -- nothing demotes a mismatch to a failure")
    if not re.search(r"^\s+passed\s*=\s*False\s*$", m.group("body"), re.M):
        return ("the fingerprint hard-fail site prints a message but never sets "
                "`passed = False` -- a red message with a green verdict, which "
                "is the exact defect this contract exists to prevent")
    return None


def _check_exc_is_failure(smoke_src: str) -> str | None:
    """A raising fingerprint check must set _fp_hard_fail, not be swallowed."""
    m = re.search(
        r"_fp_lines,\s*_fp_bad\s*=\s*_fingerprint\.check_fingerprint.*?"
        r"except\s+Exception\s+as\s+\w+\s*:(?P<body>.*?)\n(?=\s{0,8}\S)",
        smoke_src, re.S)
    if not m:
        return ("could not find the per-mission check_fingerprint call and its "
                "except handler in run_smoke.py")
    if not re.search(r"_fp_hard_fail\s*=\s*True", m.group("body")):
        return ("the per-mission fingerprint check swallows exceptions without "
                "setting `_fp_hard_fail = True` -- an exe whose fingerprint "
                "could not be checked would still be allowed to PASS")
    return None


def _check_import_fatal(smoke_src: str) -> str | None:
    """A broken fingerprint import must abort, not degrade to a silent skip."""
    m = re.search(r"if\s+_fingerprint\s+is\s+None\s*:(?P<body>.*?)\n(?=\s{0,4}if\s+_fingerprint\s+is\s+not\s+None)",
                  smoke_src, re.S)
    if not m:
        return ("run_smoke.py has no `if _fingerprint is None:` guard before "
                "the fingerprint block -- a failed import silently disables the "
                "whole check and the run still prints PASS")
    body = m.group("body")
    if "_fp_require" not in body or not re.search(r"sys\.exit\(", body):
        return ("the `_fingerprint is None` guard does not abort via sys.exit() "
                "when _fp_require is set -- breaking the import is a 3-line way "
                "to disable the fingerprint gate and still get a PASS")
    return None


def _check_docs_honest(sources: dict[str, str]) -> str | None:
    """Flag "advisory" claims ONLY where they describe the fingerprint check.

    run_smoke.py has several genuinely-advisory checks (MC2_SMOKE_REQUIRE_FRESH,
    _REQUIRE_VISUAL, _REQUIRE_DIAG_STATE) whose comments correctly say "advisory
    by default". A blanket phrase ban would fail on those correct comments and
    pressure a future lane into rewriting accurate documentation, so the phrase
    only counts when its surrounding lines are talking about the fingerprint.
    """
    hits = []
    for name, src in sources.items():
        lines = src.splitlines()

        # Rule A: opt-IN framing of the knob, anywhere in the file.
        for i, line in enumerate(lines):
            if OPT_IN_FRAMING.search(line):
                hits.append(f"{name}:{i + 1}: opt-IN framing "
                            f"'{FP_KNOB}=1' (the knob is opt-OUT; the check is "
                            f"already fatal by default)")

        # Rule B: a comment paragraph that names the knob calling it advisory.
        # Paragraph = a contiguous run of comment lines.
        para: list[tuple[int, str]] = []

        def _flush(para: list[tuple[int, str]]) -> None:
            if not para:
                return
            text = "\n".join(t for _, t in para)
            if FP_KNOB in text and ADVISORY_WORD.search(text):
                hits.append(f"{name}:{para[0][0] + 1}: comment paragraph names "
                            f"{FP_KNOB} and calls the check advisory")

        for i, line in enumerate(lines):
            if line.lstrip().startswith("#"):
                para.append((i, line))
            else:
                _flush(para)
                para = []
        _flush(para)
    if hits:
        return ("source text claims the fingerprint check is advisory, but it "
                "is fatal by default -- stale comments mislead the next lane "
                "into trusting or distrusting a verdict wrongly: "
                + "; ".join(hits))
    return None


def _check_behaviour() -> str | None:
    """Exercise the real compare function -- static checks alone can't prove it."""
    sys.path.insert(0, str(ROOT))
    try:
        from scripts.smoke_lib import fingerprint as fp
    except Exception as exc:  # pragma: no cover
        return f"cannot import scripts.smoke_lib.fingerprint: {exc!r}"
    good = "abc123def456"
    cases = [
        ("mismatch", {"sha": "0" * 12, "dirty": 0}, good, True),
        ("absent", None, good, True),
        ("match", {"sha": good, "dirty": 0}, good, False),
    ]
    for label, parsed, expected_sha, want_hard in cases:
        _lines, hard = fp.check_fingerprint(parsed, expected_sha)
        if bool(hard) is not want_hard:
            return (f"check_fingerprint({label}) returned hard_fail={hard!r}, "
                    f"expected {want_hard!r}")
    return None


def run_checks(smoke_src: str, fp_src: str, with_behaviour: bool = True):
    """Return a list of (name, error_or_None)."""
    results = [
        ("fp_default_on", _check_default_on(smoke_src)),
        ("fp_clears_verdict", _check_clears_verdict(smoke_src)),
        ("fp_exc_is_failure", _check_exc_is_failure(smoke_src)),
        ("fp_import_fatal", _check_import_fatal(smoke_src)),
        ("fp_docs_honest", _check_docs_honest(
            {"run_smoke.py": smoke_src, "smoke_lib/fingerprint.py": fp_src})),
    ]
    if with_behaviour:
        results.append(("fp_behaviour", _check_behaviour()))
    return results


# --------------------------------------------------------------------------
# Selftest: mutate the source back to each pre-fix shape and require a FAIL.
# --------------------------------------------------------------------------

def _mut_default_off(src: str) -> str:
    return src.replace(
        'os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1") != "0"',
        'os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT") == "1"')


def _mut_no_verdict_clear(src: str) -> str:
    m = re.search(r"(if\s+_fp_require\s+and\s+_fp_hard_fail\s*:.*?)\n(\s+)passed\s*=\s*False",
                  src, re.S)
    if not m:
        raise RuntimeError("selftest mutation target (passed = False) not found")
    return src[:m.start(2)] + m.group(2) + "pass" + src[m.end(0):]


def _mut_exc_swallowed(src: str) -> str:
    return src.replace("                _fp_hard_fail = True\n", "", 1)


def _mut_import_skip(src: str) -> str:
    m = re.search(r"\n(\s{4}if\s+_fingerprint\s+is\s+None\s*:.*?)\n(?=\s{4}if\s+_fingerprint\s+is\s+not\s+None)",
                  src, re.S)
    if not m:
        raise RuntimeError("selftest mutation target (import guard) not found")
    return src[:m.start(1)] + src[m.end(1):]


def _mut_dishonest_doc(src: str) -> str:
    """Restore the exact pre-fix comment (trips Rule A and Rule B)."""
    return src.replace(
        "# Deploy-fingerprint check. DEFAULT = HARD FAIL on mismatch/absence",
        "# Deploy-fingerprint check (advisory by default; "
        "MC2_SMOKE_REQUIRE_FINGERPRINT=1\n# = hard fail)", 1)


SELFTEST_CASES = [
    ("fp_default_on", "flip the gate back to opt-in", _mut_default_off, "smoke"),
    ("fp_clears_verdict", "print the message but not fail", _mut_no_verdict_clear, "smoke"),
    ("fp_exc_is_failure", "swallow the check exception", _mut_exc_swallowed, "smoke"),
    ("fp_import_fatal", "skip silently on broken import", _mut_import_skip, "smoke"),
    ("fp_docs_honest", "reintroduce 'advisory by default'", _mut_dishonest_doc, "smoke"),
]


def selftest() -> int:
    smoke_src = RUN_SMOKE.read_text(encoding="utf-8")
    fp_src = FP_LIB.read_text(encoding="utf-8")

    # Sanity: the real tree must pass, otherwise the selftest proves nothing.
    baseline = run_checks(smoke_src, fp_src)
    baseline_bad = [(n, e) for n, e in baseline if e]
    if baseline_bad:
        print("[selftest] ABORT: the real tree already fails; fix the contract "
              "before trusting the injection results:", file=sys.stderr)
        for n, e in baseline_bad:
            print(f"[selftest]   {n}: {e}", file=sys.stderr)
        return 1
    print(f"[selftest] baseline: real tree passes all {len(baseline)} checks")

    failures = 0
    for name, desc, mutate, target in SELFTEST_CASES:
        try:
            mutated_smoke = mutate(smoke_src) if target == "smoke" else smoke_src
            mutated_fp = fp_src if target == "smoke" else mutate(fp_src)
        except RuntimeError as exc:
            print(f"[selftest] FAIL {name}: could not inject ({exc})", file=sys.stderr)
            failures += 1
            continue
        if mutated_smoke == smoke_src and mutated_fp == fp_src:
            print(f"[selftest] FAIL {name}: injection was a no-op -- the "
                  f"mutation target no longer exists, so this check is "
                  f"UNPROVEN", file=sys.stderr)
            failures += 1
            continue
        res = dict(run_checks(mutated_smoke, mutated_fp, with_behaviour=False))
        if res.get(name):
            print(f"[selftest] FIRES  {name}  (injected: {desc})")
        else:
            print(f"[selftest] FAIL   {name}: injected '{desc}' and the check "
                  f"still PASSED -- this guard does not actually work",
                  file=sys.stderr)
            failures += 1

    if failures:
        print(f"\n[selftest] {failures} guard(s) did not fire on bad input", file=sys.stderr)
        return 1
    print(f"\n[selftest] OK: all {len(SELFTEST_CASES)} guards fire on injected regressions")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true",
                    help="inject each regression and require the guard to fire")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    for path in (RUN_SMOKE, FP_LIB):
        if not path.is_file():
            print(f"[smoke-fingerprint-honesty] FAIL: missing {path}", file=sys.stderr)
            return 1

    results = run_checks(RUN_SMOKE.read_text(encoding="utf-8"),
                         FP_LIB.read_text(encoding="utf-8"))
    bad = [(n, e) for n, e in results if e]
    for name, err in results:
        if err:
            print(f"[smoke-fingerprint-honesty] FAIL {name}: {err}", file=sys.stderr)
        elif not args.quiet:
            print(f"[smoke-fingerprint-honesty] ok   {name}")
    if bad:
        print(f"\n[smoke-fingerprint-honesty] {len(bad)} check(s) failed: "
              f"run_smoke.py could print PASS on an unverified exe.", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"[smoke-fingerprint-honesty] PASS ({len(results)} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
                     mismatch / absent / dirty commit / differing exe bytes
  fp_reference_wired the selected reference reaches logs, comparison and reports

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
from unittest.mock import patch

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
REFERENCE_PRINT_RE = re.compile(
    r'print\(\s*"\[runner\] \[DEPLOY_FINGERPRINT\] reference: "\s*'
    r'\+\s*_fingerprint\.describe_reference\(_fp_reference\)\s*,\s*file=sys\.stderr\s*\)')


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
    # There is more than one `if _fp_require and _fp_hard_fail:` site (one
    # feeds the written report, one clears the verdict). At least one must
    # clear `passed`, so check them all rather than only the first.
    bodies = [m.group("body") for m in re.finditer(
        r"if\s+_fp_require\s+and\s+_fp_hard_fail\s*:(?P<body>.*?)\n(?=\s{0,4}\S)",
        smoke_src, re.S)]
    if not bodies:
        return ("no `if _fp_require and _fp_hard_fail:` verdict site in "
                "run_smoke.py -- nothing demotes a mismatch to a failure")
    if not any(re.search(r"^\s+passed\s*=\s*False\s*$", b, re.M) for b in bodies):
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


def _check_report_wired(smoke_src: str) -> str | None:
    """run_smoke must actually FEED the fingerprint verdict into the report.

    _check_report_honest proves the renderer obeys run_failures; this proves
    run_smoke passes them. Both are needed: a correct renderer nobody calls
    correctly is how the report said PASS while the exit code said 1.
    """
    if not re.search(r"_run_failures\s*(:\s*list\[str\]\s*)?=\s*\[\]", smoke_src):
        return ("run_smoke.py never builds a _run_failures list -- run-level "
                "failures cannot reach the written report")
    if not re.search(r"if\s+_fp_require\s+and\s+_fp_hard_fail\s*:\s*\n\s*_run_failures\.append",
                     smoke_src):
        return ("the fingerprint verdict is not appended to _run_failures, so "
                "report.md/report.json can still record PASS for a run that "
                "smoked the wrong binary")
    for fn in ("render_markdown", "render_json"):
        m = re.search(r"report\.%s\((?P<args>.*?)\)\s*\n" % fn, smoke_src, re.S)
        if not m or "run_failures" not in m.group("args"):
            return (f"report.{fn}(...) is called without run_failures -- the "
                    f"written report ignores the fingerprint verdict")
        if "fingerprint=_fp_record" not in m.group("args"):
            return f"report.{fn}(...) drops the selected fingerprint reference and verdict text"
    # Ordering: the failure list must be built BEFORE the report is rendered.
    if smoke_src.index("_run_failures.append") > smoke_src.index("report.render_markdown("):
        return ("_run_failures is populated AFTER report.render_markdown() -- "
                "the report is rendered before the verdict exists, which is "
                "the original defect")
    return None


def _check_reference_wired(smoke_src: str) -> str | None:
    """An override must reach the compare, log and report without changing the gate."""
    if not re.search(r"reference=_fp_reference,\s*exe_path=args\.exe", smoke_src):
        return "fingerprint comparison drops the selected reference or deployed exe path"
    if not REFERENCE_PRINT_RE.search(smoke_src):
        return "runner log omits the selected fingerprint reference"
    if "reference: {_fp_reference_text}" not in smoke_src:
        return "kept mission log omits the selected fingerprint reference"
    if not re.search(r'"reference": _fp_reference.*?"checks": _fp_checks', smoke_src, re.S):
        return "report record omits the selected reference or per-mission verdict text"
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
    commit = {"mode": "commit", "requested": good, "sha": good + "0" * 28}
    for parsed, want in (({"sha": good, "dirty": 0}, False),
                         ({"sha": "0" * 12, "dirty": 0}, True),
                         ({"sha": good, "dirty": 1}, True), (None, True)):
        _lines, hard = fp.check_fingerprint(parsed, None, reference=commit)
        if hard != want:
            return f"commit reference hard_fail={hard!r}, expected {want!r} for {parsed!r}"
    exe = {"mode": "exe", "path": "reference.exe", "sha256": "a" * 64}
    for digest, parsed, want in (("a" * 64, {"sha": good, "dirty": 0}, False),
                                 ("b" * 64, {"sha": good, "dirty": 0}, True),
                                 ("a" * 64, None, True)):
        with patch.object(fp, "sha256_file", return_value=digest):
            _lines, hard = fp.check_fingerprint(parsed, None, reference=exe,
                                                exe_path="deployed.exe")
        if hard != want:
            return f"exe byte reference hard_fail={hard!r}, expected {want!r}"
    return None


def _check_report_honest() -> str | None:
    """The written REPORT must not say PASS when the run is invalid.

    The exit code being right is not enough: report.md / report.json are what
    get quoted, and get_latest_smoke_report() serves them. This drives the real
    renderers with an all-missions-passed row set plus a run-level failure and
    requires both artifacts to say FAIL.
    """
    sys.path.insert(0, str(ROOT))
    try:
        from scripts.smoke_lib import report as rep
        from scripts.smoke_lib.gates import Verdict
        from scripts.smoke_lib.logparse import LogSummary
    except Exception as exc:  # pragma: no cover
        return f"cannot import scripts.smoke_lib.report: {exc!r}"

    try:
        row = rep.Row(stem="mc2_01",
                      verdict=Verdict(passed=True, buckets=[], details=[]),
                      summary=LogSummary(), destroys_delta=0)
    except Exception as exc:
        return f"cannot construct a synthetic passing Row: {exc!r}"

    kw = dict(tier="tier1", profile="stock", timestamp="T")
    fail = ["DEPLOY_FINGERPRINT: wrong binary"]

    md = rep.render_markdown([row], run_failures=fail, **kw)
    if "result=PASS" in md:
        return ("report.md says result=PASS despite a run-level failure "
                "(fingerprint mismatch) -- the artifact readers quote would "
                "still certify a run on the wrong binary")
    js = rep.render_json([row], run_failures=fail, **kw)
    if js.get("overall_result") != "FAIL":
        return (f"report.json overall_result={js.get('overall_result')!r} "
                f"despite a run-level failure; expected 'FAIL'")
    if not js.get("run_failures"):
        return "report.json drops run_failures, hiding WHY the run is invalid"

    # And the clean case must still read PASS, or the check is vacuous.
    md_ok = rep.render_markdown([row], run_failures=[], **kw)
    if "result=PASS" not in md_ok:
        return ("a clean run no longer renders result=PASS -- the honesty fix "
                "over-fires and would mark good runs invalid")
    reference = {"mode": "exe", "path": "pre.exe", "sha256": "a" * 64}
    record = {"reference": reference, "description": "--ref-exe pre.exe sha256=" + "a" * 64,
              "required": True, "hard_fail": True,
              "checks": [{"mission": "mc2_01", "hard_fail": True,
                          "lines": ["[DEPLOY_FINGERPRINT] MISMATCH: byte identity"]}]}
    md = rep.render_markdown([row], run_failures=fail, fingerprint=record, **kw)
    js = rep.render_json([row], run_failures=fail, fingerprint=record, **kw)
    if record["description"] not in md or record["checks"][0]["lines"][0] not in md:
        return "report.md drops the fingerprint reference or verdict text"
    if js.get("fingerprint") != record:
        return "report.json drops the fingerprint reference or verdict text"
    return None


def run_checks(smoke_src: str, fp_src: str, with_behaviour: bool = True):
    """Return a list of (name, error_or_None)."""
    results = [
        ("fp_default_on", _check_default_on(smoke_src)),
        ("fp_clears_verdict", _check_clears_verdict(smoke_src)),
        ("fp_exc_is_failure", _check_exc_is_failure(smoke_src)),
        ("fp_import_fatal", _check_import_fatal(smoke_src)),
        ("fp_report_wired", _check_report_wired(smoke_src)),
        ("fp_reference_wired", _check_reference_wired(smoke_src)),
        ("fp_docs_honest", _check_docs_honest(
            {"run_smoke.py": smoke_src, "smoke_lib/fingerprint.py": fp_src})),
    ]
    if with_behaviour:
        results.append(("fp_behaviour", _check_behaviour()))
        results.append(("fp_report_honest", _check_report_honest()))
    return results


# --------------------------------------------------------------------------
# Selftest: mutate the source back to each pre-fix shape and require a FAIL.
# --------------------------------------------------------------------------

def _mut_default_off(src: str) -> str:
    return src.replace(
        'os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1") != "0"',
        'os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT") == "1"')


def _mut_no_verdict_clear(src: str) -> str:
    """Remove the fingerprint block's `passed = False`.

    Anchored on the HARD FAIL message, NOT on `if _fp_require and
    _fp_hard_fail:`: there are two such `if`s, and a lazy match from the first
    one reaches past it to the VEG-SMOKE-FLOOR block's `passed = False`, so the
    injection silently mutilated an unrelated guard and left the fingerprint
    one intact. The selftest caught that -- it reported the guard as not
    firing, which was true of the injection, not of the guard.
    """
    m = re.search(r"(HARD FAIL: smoke ran an exe.*?)\n(\s+)passed\s*=\s*False",
                  src, re.S)
    if not m:
        raise RuntimeError("selftest mutation target (fingerprint passed=False) not found")
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


def _mut_report_unwired(src: str) -> str:
    """Drop run_failures from the report calls (the pre-fix shape)."""
    return re.sub(r",?\s*\n\s*run_failures=_run_failures", "", src)


def _mut_reference_unwired(src: str) -> str:
    return src.replace("reference=_fp_reference, exe_path=args.exe", "exe_path=args.exe")


def _mut_reference_print_removed(src: str) -> str:
    return REFERENCE_PRINT_RE.sub("pass", src, count=1)


def _mut_report_reference_dropped(src: str) -> str:
    return src.replace(", fingerprint=_fp_record", "")


SELFTEST_CASES = [
    ("fp_reference_wired", "remove the runner's reference print but keep report description",
     _mut_reference_print_removed, "smoke"),
    ("fp_report_wired", "drop the reference and verdict text from reports",
     _mut_report_reference_dropped, "smoke"),
    ("fp_reference_wired", "drop the fingerprint reference override",
     _mut_reference_unwired, "smoke"),
    ("fp_report_wired", "stop feeding the verdict to the report",
     _mut_report_unwired, "smoke"),
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

#!/usr/bin/env python3
"""REGISTRY-PARSER-TRUTH-1: does docs/flags.auto.yaml tell the truth about which
gates are REGISTERED?

WHY THIS EXISTS AND WHY IT IS NOT check-flag-registry-drift.sh
--------------------------------------------------------------
The tracked docs/flags.auto.yaml asserted `registered: no` about 73 gates that
are registered, and survived that way from 2026-07-23 to 2026-07-31.  Two guards
could in principle have caught it, and neither did:

  * check-flag-registry-drift.sh regenerates the file and diffs it.  It CANNOT
    catch this class: it regenerates with the same parser that produced the
    falsehood, so a wrong-but-stable generator reads as "up to date".  It also
    sweeps the WORKING TREE for getenv() sites and records `file:line`, so any
    lane's uncommitted C++ edit that shifts a line number makes it red.  On a
    shared worktree with several lanes committing per hour that is a
    permanently-red row, and a row that is red every day teaches everyone to
    skip red.  It stays advisory, for that reason, stated in
    scripts/manual-checks.allowlist.

  * check-env-registry.sh proves which gate NAMES appear in the header.  It
    never reads flags.auto.yaml at all.

So this check grades the ONE claim that was false, against the ONE source that
is authoritative for it: every `registered:` / `default:` / `kind:` verdict in
the committed YAML must agree with a strict parse of
RenderCore/RendererFeatureRegistry.h.

It deliberately ignores `referenced_at`, `in_smoke_passthrough` and
`documented_in_tier1_env_vars` -- those depend on the working tree and on two
other documents, and folding them in here would reintroduce exactly the
churn that keeps the drift check advisory.  Narrow and always-actionable beats
broad and ignored.

Usage:
    py -3 scripts/check-flag-registry-truth.py [--quiet]
    py -3 scripts/check-flag-registry-truth.py --selftest
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from registry_parse import (RegistryParseError, parse_registry,  # noqa: E402
                            parse_registry_text)
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

YAML_PATH = os.path.join(REPO_ROOT, "docs", "flags.auto.yaml")
HEADER_PATH = os.path.join(REPO_ROOT, "RenderCore", "RendererFeatureRegistry.h")
HEADER_REL = "RenderCore/RendererFeatureRegistry.h"


def registry_from_head():
    """Parse the registry AS COMMITTED, not as it sits in the working tree.

    BOTH SIDES OF THIS COMPARISON MUST BE COMMITTED ARTIFACTS.  docs/flags.auto.yaml
    is tracked; if it were graded against the WORKING-TREE header then whichever
    lane happens to have an uncommitted gate row makes this check red for
    everyone else, and "fixing" it would mean regenerating the tracked doc with
    another lane's unlanded work baked in.  That is not hypothetical -- it
    happened during this check's first run: a lane had MC2_MECH_LIGHT_FRESHNESS
    added to the header and uncommitted, and the working-tree grade demanded a
    regeneration that would have published it.

    This is the same reasoning scripts/check-contract-coverage.py applies when it
    grades HEAD rather than the index: the question is "would every clone of this
    commit see agreement?", and only HEAD can answer it.

    Returns (registry, source_label).  Falls back to the on-disk file when git is
    unavailable (a tarball checkout), and says so.
    """
    try:
        r = subprocess.run(["git", "show", "HEAD:" + HEADER_REL],
                           cwd=REPO_ROOT, capture_output=True, text=True,
                           encoding="utf-8", timeout=20,
                           stdin=subprocess.DEVNULL, **console_child.child_kwargs(redirected=True))
        if r.returncode == 0 and r.stdout:
            return parse_registry_text(r.stdout, "HEAD:" + HEADER_REL), "HEAD"
    except (OSError, subprocess.SubprocessError):
        pass
    return parse_registry(HEADER_PATH), "working tree (git unavailable)"

_NAME_RE = re.compile(r"^  - name: (\S+)\s*$")
_FIELD_RE = re.compile(r"^    (\w+): (.*?)\s*$")


def load_yaml_flags(path: str) -> dict:
    """{name: {field: raw_string}} -- a deliberate 3-line reader, no PyYAML.

    The generator emits a fixed two-level shape; parsing it with the real YAML
    library would ALSO reinterpret `yes`/`no`/`on`/`off` as booleans, which is
    the last thing a check about the literal token `registered: no` should do.
    """
    out, cur = {}, None
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = _NAME_RE.match(line)
            if m:
                cur = m.group(1)
                out[cur] = {}
                continue
            m = _FIELD_RE.match(line)
            if m and cur:
                out[cur].setdefault(m.group(1), m.group(2))
    return out


def check(yaml_path: str = YAML_PATH, header_path=None) -> list:
    """Return a list of human-readable problems (empty == the doc is true)."""
    problems: list = []

    try:
        if header_path is None:
            reg, _src = registry_from_head()
        else:
            reg = parse_registry(header_path)
    except RegistryParseError as exc:
        # Refuse rather than grade against a partial registry -- grading the
        # doc against a broken parse is how the falsehood got written.
        return ["registry did not parse, so this doc cannot be graded: %s" % exc]

    if not os.path.exists(yaml_path):
        return ["%s does not exist -- regenerate: py -3 scripts/flag-registry-auto-gen.py"
                % yaml_path]

    flags = load_yaml_flags(yaml_path)
    if not flags:
        return ["%s parsed to ZERO flag entries -- it is empty, truncated, or its "
                "shape changed; refusing to call that agreement" % yaml_path]

    registered = {e["envVar"]: e for e in reg.values() if e["envVar"]}

    # (1) every registered gate must be present AND say so
    for name, entry in sorted(registered.items()):
        row = flags.get(name)
        if row is None:
            problems.append(
                "%s is registered in RendererFeatureRegistry.h (%s, %s) but is "
                "ABSENT from the doc" % (name, entry["featureId"], entry["table"]))
            continue
        if row.get("registered") != "yes":
            problems.append(
                "%s says `registered: %s` but IS registered (%s in %s). This is "
                "the exact falsehood REGISTRY-PARSER-TRUTH-1 found 73 of."
                % (name, row.get("registered"), entry["featureId"], entry["table"]))
        want_default = "on" if entry["defaultOn"] else "off"
        if row.get("default") != want_default:
            problems.append(
                "%s says `default: %s`, registry says %s"
                % (name, row.get("default"), want_default))
        if row.get("kind") != entry["kind"]:
            problems.append(
                "%s says `kind: %s`, registry says %s"
                % (name, row.get("kind"), entry["kind"]))

    # (2) and nothing may claim registration it does not have
    for name, row in sorted(flags.items()):
        if row.get("registered") == "yes" and name not in registered:
            problems.append(
                "%s says `registered: yes` but has no row in "
                "RendererFeatureRegistry.h" % name)

    return problems


def selftest() -> int:
    """Falsify the doc four ways in a temp copy; require detection of each.

    Built by CORRUPTING THE REAL docs/flags.auto.yaml, not a fixture: a fixture
    stops resembling the file it stands for, and a guard whose fixture has
    drifted passes while blind.
    """
    import tempfile

    if not os.path.exists(YAML_PATH):
        print("SELF-TEST INCONCLUSIVE: %s missing" % YAML_PATH)
        return 1
    text = open(YAML_PATH, encoding="utf-8").read()

    try:
        reg, src = registry_from_head()
    except RegistryParseError as exc:
        print("SELF-TEST INCONCLUSIVE: registry will not parse: %s" % exc)
        return 1
    print("  ..    grading against the registry as of %s" % src)
    anchor = next((e["envVar"] for e in reg.values()
                   if e["envVar"] and "  - name: %s\n" % e["envVar"] in text), None)
    if anchor is None:
        print("SELF-TEST INCONCLUSIVE: no registered gate found in the doc to "
              "corrupt -- re-anchor this test")
        return 1

    head = "  - name: %s\n" % anchor
    i = text.index(head)
    j = text.index("  - name: ", i + len(head))
    block = text[i:j]

    arms = [
        ("a registered gate demoted to `registered: no`",
         text.replace(block, block.replace("    registered: yes",
                                           "    registered: no", 1), 1),
         "IS registered"),
        ("a registered gate's default flipped",
         text.replace(block, re.sub(r"    default: (on|off)",
                                    lambda m: "    default: " + ("off" if m.group(1) == "on" else "on"),
                                    block, count=1), 1),
         "registry says"),
        ("a registered gate deleted from the doc outright",
         text.replace(block, "", 1), "ABSENT from the doc"),
        ("an unregistered gate claiming `registered: yes`",
         text + "  - name: MC2_SELFTEST_NOT_REAL\n    default: unknown\n"
                "    declared_in: null\n    referenced_at: []\n"
                "    registered: yes\n    in_smoke_passthrough: no\n"
                "    documented_in_tier1_env_vars: no\n",
         "no row in RendererFeatureRegistry.h"),
    ]

    failures = 0
    if check():
        print("  FAIL  the LIVE doc does not agree with the registry -- fix that "
              "first; the arms below cannot be trusted")
        return 1
    print("  OK    the live doc agrees with the registry on every registered gate")

    tmpdir = tempfile.mkdtemp(prefix="mc2_flagtruth_")
    tmp = os.path.join(tmpdir, "flags.auto.yaml")
    try:
        for label, blob, expect in arms:
            if blob == text:
                print("  FAIL  arm %r changed nothing -- BLIND" % label)
                failures += 1
                continue
            with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(blob)
            probs = check(yaml_path=tmp)
            if any(expect in p for p in probs):
                print("  OK    caught: %s" % label)
            else:
                print("  FAIL  MISSED %s (problems: %s)" % (label, probs[:2]))
                failures += 1
    finally:
        try:
            os.remove(tmp)
            os.rmdir(tmpdir)
        except OSError:
            pass

    if failures:
        print("SELF-TEST FAIL: %d arm(s) did not behave" % failures)
        return 1
    print("SELF-TEST PASS: the doc is graded against the registry, and every way "
          "of falsifying a `registered:` verdict is caught")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)

    if a.selftest:
        return selftest()

    problems = check()
    if problems:
        print("[flag-registry-truth] FAIL: %d disagreement(s) between "
              "docs/flags.auto.yaml and RenderCore/RendererFeatureRegistry.h:"
              % len(problems))
        for p in problems[:40]:
            print("  " + p)
        if len(problems) > 40:
            print("  ... and %d more" % (len(problems) - 40))
        print("  Regenerate: py -3 scripts/flag-registry-auto-gen.py")
        return 1
    if not a.quiet:
        print("[flag-registry-truth] OK: docs/flags.auto.yaml agrees with the "
              "registry on every registered gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())

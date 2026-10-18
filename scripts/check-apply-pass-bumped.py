#!/usr/bin/env python3
"""check-apply-pass-bumped.py -- APPLY-STATE-REGISTRATION-CHECK-1 completeness gate.

Closes the gap no offline doctest can see: a doctest can verify the ApplyPassId
vocabulary is internally consistent (names unique, top-level apply => concrete
pipeline, post-process apply => closed sub-stage/island chain), but it CANNOT tell
whether each enumerator is actually WIRED to a runtime apply-bump call site. An
ApplyPassId added to the enum but never bumped (half-shipped wiring) compiles and
passes every doctest, yet its per-pass counter stays silently 0 forever.

This gate is a FULL-SCAN completeness invariant (NOT diff-based): it parses every
enumerator from `enum class ApplyPassId` in RenderCore/frame_executor.h (excluding
Count) and asserts each one appears in EXACTLY ONE apply-bump call site across the
engine TUs (GameOS/gameos/*.cpp). Two accepted bump forms:

  * 5 PostProcess sub-stages (gos_postprocess.cpp):
        applyStateBump(...ApplyPassId::X)
  * 3 top-level (gameos_graphics.cpp / gos_static_prop_batcher.cpp):
        mc2_framegraph_executor_bump_apply_state((unsigned)...ApplyPassId::X)

FAIL (exit 1) if any enumerator has 0 bump sites (enum added but never wired) or
>1 (double-wired). The applyStateBump() / mc2_framegraph_executor_bump_apply_state()
DEFINITIONS take an ApplyPassId/id parameter (not ApplyPassId::Name) so they do not
count as bump sites; only the ::Name call sites are matched.

Mirrors the RAW-GL-*-DIFF-GATE family structure (scripts/check-raw-gl-depthfunc.py)
but runs as a completeness assertion every CI pass.

Pure stdlib. Exit 0 when all enumerators have exactly 1 bump site; else exit 1.
"""
import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ENUM_HEADER = os.path.join(REPO_ROOT, "RenderCore", "frame_executor.h")
ENGINE_TU_DIR = os.path.join(REPO_ROOT, "GameOS", "gameos")


def parse_enumerators():
    """Parse ApplyPassId enumerator names (excluding Count) from frame_executor.h."""
    with open(ENUM_HEADER, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = re.search(r"enum\s+class\s+ApplyPassId\s*:[^\{]*\{(.*?)\};", text, re.DOTALL)
    if not m:
        m = re.search(r"enum\s+class\s+ApplyPassId\s*\{(.*?)\};", text, re.DOTALL)
    if not m:
        print("[check-apply-pass-bumped] ERROR: could not locate "
              "`enum class ApplyPassId` in %s" % ENUM_HEADER, file=sys.stderr)
        return None
    body = m.group(1)
    # Strip comments BEFORE splitting on commas: a comma inside a trailing `// ...`
    # comment would otherwise split mid-comment and swallow the NEXT enumerator name
    # (a comma in a Water comment otherwise drops Shadow).
    body = re.sub(r"//.*$", "", body, flags=re.MULTILINE)   # strip line comments
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)   # strip block comments
    names = []
    for raw in body.split(","):
        line = raw.strip()
        if not line:
            continue
        # enumerator may have an explicit value: "PostProcessEdgeFog = 0"
        name = line.split("=")[0].strip()
        if not re.match(r"^[A-Za-z_]\w*$", name):
            continue
        if name == "Count":
            continue
        names.append(name)
    return names


def engine_tus():
    for fn in sorted(os.listdir(ENGINE_TU_DIR)):
        if fn.endswith(".cpp"):
            yield os.path.join(ENGINE_TU_DIR, fn)


def strip_comments(line):
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r"/\*.*?\*/", "", line)
    return line


def count_bump_sites(names):
    """Return {name: [ (file, lineno, text), ... ]} of bump call sites."""
    # Build one regex per enumerator matching either accepted bump form.
    # Both forms end in `ApplyPassId::<Name>` after a `(` (call), so we require a
    # preceding bump-function token to avoid matching the enum decl / unrelated refs.
    site_re = {}
    for n in names:
        site_re[n] = re.compile(
            r"(?:applyStateBump|mc2_framegraph_executor_bump_apply_state)\s*\("
            r".*?ApplyPassId::%s\b" % re.escape(n))

    sites = {n: [] for n in names}
    for path in engine_tus():
        rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for i, raw in enumerate(f, 1):
                scan = strip_comments(raw)
                if "ApplyPassId::" not in scan:
                    continue
                for n in names:
                    if site_re[n].search(scan):
                        sites[n].append((rel, i, raw.strip()))
    return sites


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter; offenders still printed")
    args = ap.parse_args()

    names = parse_enumerators()
    if names is None:
        return 1
    if not names:
        print("[check-apply-pass-bumped] ERROR: parsed 0 ApplyPassId enumerators",
              file=sys.stderr)
        return 1

    sites = count_bump_sites(names)

    failures = []
    for n in names:
        c = len(sites[n])
        if c != 1:
            failures.append((n, c, sites[n]))

    if not args.quiet:
        print("[check-apply-pass-bumped] %d ApplyPassId enumerator(s); "
              "each must have exactly 1 apply-bump call site" % len(names))

    if not failures:
        if not args.quiet:
            for n in names:
                f, ln, _ = sites[n][0]
                print("  OK  %-26s %s:%d" % (n, f, ln))
            print("[check-apply-pass-bumped] OK: all %d enumerators wired exactly once"
                  % len(names))
        return 0

    for n, c, where in failures:
        if c == 0:
            print("APPLY-PASS-UNWIRED %s -- enum added but no apply-bump call site "
                  "(half-shipped wiring)" % n, file=sys.stderr)
        else:
            print("APPLY-PASS-DOUBLE-WIRED %s -- %d bump sites (expected 1):"
                  % (n, c), file=sys.stderr)
            for f, ln, txt in where:
                print("    %s:%d  %s" % (f, ln, txt), file=sys.stderr)
    print("[check-apply-pass-bumped] FAIL: %d ApplyPassId enumerator(s) not wired "
          "exactly once" % len(failures), file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Fast static precheck: a tip must not ADD an unconditional bracketed log tag.

The authoritative check is scripts/check-default-build-quiet.py (runtime, at the gate). This is its offline
early warning, so the cheap class of failure (a new fprintf(stderr, "[NAME v1] ...") with no
mc2_diag::tagRequested guard) is caught in the fast precheck instead of a whole merge window later.

An added line that prints `[NAME v1]` / `[NAME]` through fprintf/printf/MC2_LOG/cerr/cout/puts passes when
  * the tag (name part) is in tests/contract/default_build_quiet_baseline.json (the quiet check's allowlist), or
  * a diag guard (tagRequested / diag_tag / tagEnabled / MC2_DIAG_VERBOSE) appears on the line or in the
    GUARD_WINDOW new-file lines above it (heuristic).
MC2_DIAGF/MC2_DIAG are self-gated macros and are not print sites here.

  py -3 tools/ungated_log_tags.py --base <ref> --head <ref>     exit 0 ok, 1 ungated tags found
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import console_child  # noqa: E402

GUARD_WINDOW = 8
BASELINE_PATH = "tests/contract/default_build_quiet_baseline.json"
SOURCE = re.compile(r"\.(cpp|h|hpp|inc|c)$")
PRINT = re.compile(r"\b(?:std::)?(?:fprintf|printf|puts|fputs)\s*\(|\bMC2_LOG\w*\s*\(|\bstd::c(?:err|out)\b")
TAG = re.compile(r"\[([A-Z][A-Z0-9_]{2,})(?:\s+v\d+)?\]")
GUARD = re.compile(r"tagRequested|diag_tag|tagEnabled|MC2_DIAG_VERBOSE")
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)")


def allowed_names(baseline_json: str) -> set[str]:
    try:
        tags = json.loads(baseline_json).get("tags", {})
    except ValueError:
        return set()
    return {k.split()[0] for k in tags}


def check_diff(diff: str, allowed: set[str]) -> list[str]:
    """Return 'path:line: [TAG]' for each ungated added print of a bracketed tag."""
    bad = []
    path = None
    window: list[str] = []
    lineno = 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            path = None if raw[4:] == "/dev/null" else raw[4:].removeprefix("b/")
            continue
        if raw.startswith("--- ") or raw.startswith("diff "):
            continue
        m = HUNK.match(raw)
        if m:
            lineno, window = int(m.group(1)), []
            continue
        if path is None or not SOURCE.search(path) or not raw or raw[0] not in " +":
            continue
        text = raw[1:]
        if raw[0] == "+" and PRINT.search(text):
            for t in TAG.finditer(text):
                name = t.group(1)
                if name in allowed:
                    continue
                if GUARD.search(text) or any(GUARD.search(w) for w in window[-GUARD_WINDOW:]):
                    continue
                bad.append(f"{path}:{lineno}: [{name}]")
        window.append(text)
        lineno += 1
    return bad


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace",
                          **console_child.child_kwargs(redirected=True)).stdout


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True)
    ap.add_argument("--head", required=True)
    a = ap.parse_args(argv)
    diff = git("diff", f"-U{GUARD_WINDOW}", "--no-color", a.base, a.head, "--", "*.cpp", "*.h", "*.hpp", "*.inc", "*.c")
    bad = check_diff(diff, allowed_names(git("show", f"{a.head}:{BASELINE_PATH}")))
    for b in bad:
        print("UNGATED LOG TAG " + b + " (guard with mc2_diag::tagRequested or list it in " + BASELINE_PATH + ")")
    print(f"ungated_log_tags: {'FAIL ' + str(len(bad)) if bad else 'PASS'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

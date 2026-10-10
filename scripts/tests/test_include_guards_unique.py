"""Two project headers must not share an include guard: whichever a translation unit includes second is silently skipped.

mclib/timing.h and GameOS/gameos/utils/timing.h both used TIMING_H, and code/ and editor/ both had OBJECTIVE_H and MAINMENU_H.
"""
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

SKIP_PREFIXES = ("3rdparty/", "tools/", "tests/", "test_scripts/", "data_tools/")
GUARD = re.compile(r"#\s*ifndef\s+(\w+)\s*$")


def _guards():
    files = subprocess.run(["git", "-C", ROOT, "ls-files", "*.h", "*.hpp"], capture_output=True, text=True,
                           **console_child.child_kwargs(redirected=True)).stdout.split("\n")
    found = collections.defaultdict(list)
    for f in files:
        if not f or f.startswith(SKIP_PREFIXES):
            continue
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            lines = [ln.strip() for ln in fh.read(6000).split("\n")]
        for i, ln in enumerate(lines):
            m = GUARD.match(ln)
            if m and i + 1 < len(lines) and re.match(r"#\s*define\s+" + m.group(1) + r"\b", lines[i + 1]):
                found[m.group(1)].append(f)
                break
    return found


def test_no_two_project_headers_share_an_include_guard():
    dups = {k: v for k, v in _guards().items() if len(v) > 1}
    assert not dups, "duplicate include guards (rename one): %r" % dups


def test_the_previously_clashing_headers_have_distinct_guards():
    g = _guards()
    assert "mclib/timing.h" in g["TIMING_H"] and "GameOS/gameos/utils/timing.h" not in g["TIMING_H"]
    assert g["EDITOR_OBJECTIVE_H"] == ["editor/Objective.h"]
    assert g["EDITOR_MAINMENU_H"] == ["editor/mainmenu.h"]

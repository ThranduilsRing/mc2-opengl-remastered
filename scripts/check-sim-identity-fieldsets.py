#!/usr/bin/env python3
"""check-sim-identity-fieldsets.py -- SIM-IDENTITY-HASH-1 field-set contract.

WHAT BREAKS IF THIS IS NOT CHECKED
==================================
The refuse-to-grade gate splits mover fields VISUAL vs NON-VISUAL in TWO places:

  * C++    mclib/sim_identity.h :: digestMoverVisual()  -- folds the VISUAL set
  * Python scripts/sim_identity.py :: VISUAL_FIELDS / NONVISUAL_FIELDS

They are independent lists that must agree. If they drift, the gate misgrades in
the DANGEROUS direction: add a field to the C++ visual digest but not to the
Python VISUAL list and a genuine visual divergence gets classified LATENT --
which GRADES. A false pass, produced by the very instrument built to stop false
passes.

That failure would be silent. Every self-test in both halves would still pass,
because each half is internally consistent. Only a cross-check catches it.

WHAT THIS DOES NOT CHECK
========================
It compares NAMES, not semantics. It cannot tell you that `hp` was correctly
called visual; it can only tell you the two halves agree about `hp`. Judging
whether the split is *right* is a human call and is argued in the header comment
of mclib/sim_identity.h.

Also: the C++ side is parsed from source text, not compiled. A field folded
through a macro or a helper this parser does not understand would be missed. The
parser therefore FAILS LOUDLY if it cannot find the functions or extracts an
implausibly small set, rather than quietly comparing empty lists -- an empty-vs-
empty "agreement" is the classic dead-check signature.

Exit 0 = agree. Exit 1 = drift (named). Exit 2 = could not parse (never a pass).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP_HEADER = ROOT / "mclib" / "sim_identity.h"
PY_MODULE = ROOT / "scripts" / "sim_identity.py"

# Map C++ MoverRecord member names -> the JSON key the collector emits, which is
# what the Python comparator keys on. The collector
# (code/sim_identity.cpp, mc2SimIdentityWriteJson) is the authority for these.
CPP_TO_JSON = {
    "team": "team",
    "px": "pos", "py": "pos", "pz": "pos",
    "rot": "rot",
    "hp": "hp",
    "gesture": "gesture",
    "animFrame": "anim",
    "status": "status",
    "target": "target",
    "brainState": "brain",
    "moveState": "mstate",
    "pathStep": "pstep",
    "pathSteps": "psteps",
}

# `id` is the join key, not a compared field, in either half.
IGNORED = {"id"}


def _fn_body(text: str, name: str) -> str:
    """Extract a brace-balanced function body by name. Returns '' if absent."""
    m = re.search(r"\b%s\s*\([^)]*\)\s*\{" % re.escape(name), text)
    if not m:
        return ""
    i = m.end() - 1
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
    return ""


def cpp_field_sets():
    """(visual_json_keys, graded_json_keys) from the C++ digest functions."""
    text = CPP_HEADER.read_text(encoding="utf-8")
    out = {}
    for fn in ("digestMoverVisual", "digestMoverGraded"):
        body = _fn_body(text, fn)
        if not body:
            raise RuntimeError("could not locate %s() in %s" % (fn, CPP_HEADER))
        keys = set()
        for member in re.findall(r"\br\.([A-Za-z_][A-Za-z0-9_]*)", body):
            if member in IGNORED:
                continue
            if member not in CPP_TO_JSON:
                raise RuntimeError(
                    "%s() folds MoverRecord member %r which this checker has no "
                    "JSON mapping for -- add it to CPP_TO_JSON (and make sure "
                    "code/sim_identity.cpp actually emits it)." % (fn, member))
            keys.add(CPP_TO_JSON[member])
        out[fn] = keys
    return out["digestMoverVisual"], out["digestMoverGraded"]


def py_field_sets():
    """(VISUAL_FIELDS, NONVISUAL_FIELDS) from the Python comparator."""
    text = PY_MODULE.read_text(encoding="utf-8")
    res = {}
    for name in ("VISUAL_FIELDS", "NONVISUAL_FIELDS"):
        m = re.search(r"^%s\s*=\s*\(([^)]*)\)" % name, text, re.M)
        if not m:
            raise RuntimeError("could not locate %s in %s" % (name, PY_MODULE))
        res[name] = set(re.findall(r'"([^"]+)"', m.group(1)))
    return res["VISUAL_FIELDS"], res["NONVISUAL_FIELDS"]


def main() -> int:
    try:
        cpp_visual, cpp_graded = cpp_field_sets()
        py_visual, py_nonvisual = py_field_sets()
    except Exception as e:
        print("[sim-identity-fieldsets] PARSE FAILED: %s" % e, file=sys.stderr)
        print("[sim-identity-fieldsets] RESULT: FAIL (could not parse -- this is "
              "NOT a pass; the contract was not checked)")
        return 2

    # Dead-check guard. Two empty sets compare equal, and that is exactly how a
    # check reports AGREE while seeing nothing.
    if len(cpp_visual) < 5 or len(py_visual) < 5:
        print("[sim-identity-fieldsets] implausibly small field set "
              "(cpp_visual=%d py_visual=%d) -- refusing to report agreement"
              % (len(cpp_visual), len(py_visual)), file=sys.stderr)
        return 2

    ok = True

    missing_in_py = cpp_visual - py_visual
    extra_in_py = py_visual - cpp_visual
    if missing_in_py:
        ok = False
        print("[sim-identity-fieldsets] FAIL: folded into the C++ VISUAL digest "
              "but ABSENT from Python VISUAL_FIELDS: %s"
              % ", ".join(sorted(missing_in_py)))
        print("    -> a divergence in these would be classified LATENT and "
              "GRADED. That is a FALSE PASS.")
    if extra_in_py:
        ok = False
        print("[sim-identity-fieldsets] FAIL: in Python VISUAL_FIELDS but NOT "
              "folded into the C++ VISUAL digest: %s"
              % ", ".join(sorted(extra_in_py)))
        print("    -> the digests would agree while Python reports a visual "
              "diff; verdict UNGRADED on a comparable pair (false refusal).")

    # The graded digest must cover visual + non-visual, i.e. everything.
    py_all = py_visual | py_nonvisual
    if cpp_graded != py_all:
        ok = False
        print("[sim-identity-fieldsets] FAIL: C++ GRADED digest fields %s != "
              "Python VISUAL+NONVISUAL %s"
              % (sorted(cpp_graded), sorted(py_all)))

    overlap = py_visual & py_nonvisual
    if overlap:
        ok = False
        print("[sim-identity-fieldsets] FAIL: field(s) in BOTH Python lists: %s"
              % ", ".join(sorted(overlap)))

    if ok:
        print("[sim-identity-fieldsets] visual=%s" % sorted(cpp_visual))
        print("[sim-identity-fieldsets] non-visual=%s" % sorted(py_nonvisual))
        print("[sim-identity-fieldsets] RESULT: PASS (C++ and Python agree on "
              "%d visual + %d non-visual fields)"
              % (len(cpp_visual), len(py_nonvisual)))
        return 0
    print("[sim-identity-fieldsets] RESULT: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())

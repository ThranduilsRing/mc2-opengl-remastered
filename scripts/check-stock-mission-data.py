#!/usr/bin/env python3
"""check-stock-mission-data.py -- STOCK-DATA-TRUTH-1 gate.

THE DEFECT THIS EXISTS TO PREVENT
---------------------------------
Eight offline tools defaulted their mission-data path to
`A:/Games/Carver5-feasibility/data/missions` and documented it as "stock
mission data". Measured against the shipped `mission.fst`, that tree differs on
**all 24 campaign maps** -- every `mc2_01..mc2_24` `.pak` AND `.fit`. Anything
run with default flags therefore graded terrain the engine never loads, and the
failure was 100% silent: the files exist, they parse, they produce plausible
numbers. One lane came within a meta-review of publishing a go/no-go on a
shoreline design computed from the wrong terrain
(`.claude/SHORELINE-V0-FALSIFY-1-STATUS.md` section 0a).

WHAT THIS GATE CHECKS
---------------------
A. STATIC (machine-independent -- this is the arm registered in
   check-contracts.sh). Parses every `tools/**/*.py` and `scripts/**/*.py` with
   `ast` and FAILS on:
     A1. any absolute path string literal, IN EXECUTABLE CODE, whose path
         components include a directory named `missions`. Docstrings are
         excluded via `ast.get_docstring`; comments never enter the AST at all
         -- so a path that appears ONLY in a comment does not satisfy this gate
         and does not trip it either. `--selftest` proves both directions.
     A2. any `add_argument("--missions-dir", ..., default=<not None>)`.
   The ban is STRUCTURAL, not a denylist of one bad string. Banning the literal
   `Carver5-feasibility` would let the identical defect return under a
   different wrong path; banning *hard-coded absolute mission dirs* cannot.
   The single allowlisted file, `tools/mc2data/stock_missions.py`, is itself
   policed: its `CANDIDATE_DIRS` must not contain a known-divergent tree.

B. MANIFEST SHAPE (machine-independent). `tools/mc2data/stock_missions.md5`
   must exist, parse, carry its provenance header, and be non-trivial.

C. DATA (machine-dependent -- run deliberately, not in check-contracts.sh).
   Every `CANDIDATE_DIRS` entry present on this machine must byte-match the
   manifest; every deploy lane's `mission.fst` must reproduce the manifest
   exactly. A lane that ever diverges FAILS here rather than being silently
   preferred by the resolver.

USAGE
-----
  py -3 scripts/check-stock-mission-data.py                # A + B + C
  py -3 scripts/check-stock-mission-data.py --no-data      # A + B only
  py -3 scripts/check-stock-mission-data.py --selftest     # prove the gate fires
  py -3 scripts/check-stock-mission-data.py --write        # regenerate manifest
  py -3 scripts/check-stock-mission-data.py --json

Exit codes: 0 pass, 2 fail, 1 usage/IO error. `--write` uses newline="\\n".
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "mc2data"))

import stock_missions as sm  # noqa: E402

SCAN_ROOTS = ("tools", "scripts")

# The one file allowed to name mission-data directories in executable code --
# it is the resolver, naming them is its job. It does not get a free pass:
# check C verifies every candidate it names, and check A1b verifies it names no
# known-divergent tree.
ALLOWLIST = {
    "tools/mc2data/stock_missions.py",
    "scripts/check-stock-mission-data.py",
}

# Trees PROVEN to differ from the shipped bytes. Must never appear as a
# resolver candidate. (Carver5-feasibility: differs on all 24 campaign maps,
# .pak and .fit, measured 2026-07-30.)
KNOWN_DIVERGENT = ("carver5-feasibility",)

# An absolute path (drive-letter, UNC, or POSIX root) with a `missions`
# component. Deliberately not anchored on any particular wrong tree.
_ABS = r"(?:[A-Za-z]:[\\/]|\\\\|/)"
MISSION_PATH_RE = re.compile(
    rf"^{_ABS}[^\n]*[\\/]missions(?:[\\/][^\n]*)?$", re.IGNORECASE)


def _rel(p: Path) -> str:
    try:
        return str(p.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return str(p).replace("\\", "/")


# ---------------------------------------------------------------------------
# A. static scan
# ---------------------------------------------------------------------------

def _docstring_nodes(tree: ast.AST) -> set[int]:
    out: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
                             ast.AsyncFunctionDef)):
            body = getattr(node, "body", None)
            if (body and isinstance(body[0], ast.Expr)
                    and isinstance(body[0].value, ast.Constant)
                    and isinstance(body[0].value.value, str)):
                out.add(id(body[0].value))
    return out


def scan_file(path: Path) -> list[str]:
    """Return findings for one python file (empty == clean)."""
    try:
        # utf-8-sig: a BOM'd file is not this gate's problem, and reporting one
        # as a mission-data defect is a false accusation (tools/terrain_gen/
        # __init__.py carries a BOM and is unrelated).
        src = path.read_text(encoding="utf-8-sig", errors="replace")
        tree = ast.parse(src, filename=str(path))
    except SyntaxError as e:
        return [f"{_rel(path)}: unparseable ({e})"]

    docs = _docstring_nodes(tree)
    found: list[str] = []

    # A1 -- absolute mission-data path literals in executable code.
    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
            continue
        if id(node) in docs:
            continue
        v = node.value.strip()
        if MISSION_PATH_RE.match(v):
            found.append(f"{_rel(path)}:{node.lineno}: hard-coded absolute "
                         f"mission-data path {v!r} in executable code")

    # A2 -- a --missions-dir argparse default other than None.
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        fn = node.func
        if not (isinstance(fn, ast.Attribute) and fn.attr == "add_argument"):
            continue
        names = [a.value for a in node.args
                 if isinstance(a, ast.Constant) and isinstance(a.value, str)]
        # DIRECTORY flags only. A bare `--missions` / positional `missions` is
        # a list of mission STEMS (mc2_01, ...) and a default there is fine --
        # matching it flagged three call sites that were never the defect.
        if not any(n.startswith("-") and
                   n.lstrip("-").replace("_", "-") in ("missions-dir", "mission-dir")
                   for n in names):
            continue
        for kw in node.keywords:
            if kw.arg == "default" and not (
                    isinstance(kw.value, ast.Constant) and kw.value.value is None):
                try:
                    shown = ast.unparse(kw.value)
                except Exception:
                    shown = "<expr>"
                found.append(f"{_rel(path)}:{node.lineno}: --missions-dir has a "
                             f"baked-in default ({shown}); it must be None so the "
                             f"verified resolver runs")
    return found


def static_scan() -> list[str]:
    findings: list[str] = []
    for r in SCAN_ROOTS:
        base = ROOT / r
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*.py")):
            rel = _rel(p)
            if rel in ALLOWLIST or "__pycache__" in rel:
                continue
            findings += scan_file(p)

    # A1b -- police the one allowlisted file.
    for cand in sm.CANDIDATE_DIRS:
        low = cand.replace("\\", "/").lower()
        for bad in KNOWN_DIVERGENT:
            if bad in low:
                findings.append(
                    f"tools/mc2data/stock_missions.py: CANDIDATE_DIRS contains "
                    f"{cand!r}, a tree PROVEN to differ from the shipped bytes")
    if not sm.CANDIDATE_DIRS:
        findings.append("tools/mc2data/stock_missions.py: CANDIDATE_DIRS is empty")
    return findings


# ---------------------------------------------------------------------------
# B. manifest shape
# ---------------------------------------------------------------------------

def manifest_shape() -> tuple[list[str], dict]:
    problems: list[str] = []
    info: dict = {}
    try:
        man = sm.load_manifest()
    except sm.StockDataError as e:
        return [str(e)], info
    hdr = sm.manifest_header()
    info["entries"] = len(man)
    info["header"] = hdr
    paks = [k for k in man if k.endswith(".pak") and "/" not in k]
    info["top_level_paks"] = len(paks)
    if len(man) < 100:
        problems.append(f"manifest has only {len(man)} entries -- looks truncated")
    if len(paks) < 20:
        problems.append(f"manifest lists only {len(paks)} top-level .pak files; "
                        f"the stock campaign alone is 24 maps")
    for key in ("fst_md5", "source_fst", "generated_by"):
        if key not in hdr:
            problems.append(f"manifest header is missing `# {key}:`")
    for k, v in man.items():
        if len(v) != 32 or not re.fullmatch(r"[0-9a-f]{32}", v):
            problems.append(f"manifest entry {k} has a non-md5 digest {v!r}")
            break
    return problems, info


# ---------------------------------------------------------------------------
# C. data verification
# ---------------------------------------------------------------------------

def data_checks() -> tuple[list[str], list[str], dict]:
    """Return (failures, notes, info)."""
    fails: list[str] = []
    notes: list[str] = []
    info: dict = {"candidates": {}, "lanes": {}}
    man = sm.load_manifest()

    for cand in sm.CANDIDATE_DIRS:
        d = Path(cand)
        if not d.is_dir():
            notes.append(f"candidate absent on this machine: {cand}")
            info["candidates"][cand] = "absent"
            continue
        ok, n, bad = sm.verify_dir(d, None, man)
        info["candidates"][cand] = {"ok": ok, "checked": n, "mismatches": len(bad)}
        if ok:
            notes.append(f"candidate VERIFIED ({n} files): {cand}")
        else:
            fails.append(f"candidate {cand} disagrees with the shipped bytes on "
                         f"{len(bad)} file(s), e.g. {bad[0]}")

    seen_any_lane = False
    for r in sm.CANDIDATE_FST_ROOTS:
        fst = Path(r) / "mission.fst"
        if not fst.is_file():
            info["lanes"][r] = "absent"
            continue
        seen_any_lane = True
        try:
            got = sm.fst_mission_digests(fst)
        except sm.StockDataError as e:
            fails.append(f"{fst}: {e}")
            continue
        diff = sorted(k for k in set(got) | set(man)
                      if got.get(k) != man.get(k))
        info["lanes"][r] = {"members": len(got), "diffs": len(diff)}
        if diff:
            fails.append(f"deployed {fst} disagrees with the committed manifest "
                         f"on {len(diff)} member(s), e.g. {diff[0]} -- the lanes "
                         f"have DIVERGED; re-run with --write and re-verify")
        else:
            notes.append(f"lane VERIFIED ({len(got)} members): {fst}")
    if not seen_any_lane:
        notes.append("no deployed mission.fst on this machine -- lane arm skipped")
    return fails, notes, info


# ---------------------------------------------------------------------------
# --write
# ---------------------------------------------------------------------------

def write_manifest(fst_path: Path | None) -> int:
    fst = fst_path or sm.find_deployed_fst()
    if fst is None:
        print("[stock-data] ERROR no deployed mission.fst found; pass --fst PATH",
              file=sys.stderr)
        return 1
    digests = sm.fst_mission_digests(fst)
    fst_md5 = sm._md5(fst)

    agree, disagree = [], []
    for r in sm.CANDIDATE_FST_ROOTS:
        other = Path(r) / "mission.fst"
        if not other.is_file():
            continue
        (agree if sm._md5(other) == fst_md5 else disagree).append(str(other))

    lines = [
        "# STOCK-DATA-TRUTH-1 -- digest of the stock mission data the engine ships.",
        "#",
        "# GENERATED FILE. Do not hand-edit.",
        "# Regenerate: py -3 scripts/check-stock-mission-data.py --write",
        "#",
        "# Authority: the deployed mission.fst. mclib/file.cpp resolves",
        "#   active mod overlay -> loose file on disk -> FastFile(.fst),",
        "# and for every mc2_* map the first two rungs miss in every lane, so the",
        "# bytes below are definitionally what the engine loads. A source tree is",
        "# usable only by matching this file.",
        "#",
        f"# source_fst: {str(fst).replace(chr(92), '/')}",
        f"# fst_md5: {fst_md5}",
        f"# byte_identical_lanes: {len(agree)}",
        f"# divergent_lanes: {len(disagree) if disagree else 0}",
        f"# entries: {len(digests)}",
        "# generated_by: scripts/check-stock-mission-data.py --write",
        "#",
        "# format: <md5>  <path relative to data/missions/>",
    ]
    for other in sorted(agree):
        lines.append(f"# lane_agrees: {other.replace(chr(92), '/')}")
    for other in sorted(disagree):
        lines.append(f"# LANE_DIVERGES: {other.replace(chr(92), '/')}")
    for k in sorted(digests):
        lines.append(f"{digests[k]}  {k}")

    sm.MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    sm.MANIFEST_PATH.write_text("\n".join(lines) + "\n",
                                encoding="utf-8", newline="\n")
    print(f"[stock-data] wrote {_rel(sm.MANIFEST_PATH)}: {len(digests)} entries "
          f"from {fst} (md5 {fst_md5[:12]}), {len(agree)} lane(s) byte-identical"
          + (f", {len(disagree)} DIVERGENT" if disagree else ""))
    return 0


# ---------------------------------------------------------------------------
# --selftest: prove the gate FIRES, and prove what does NOT satisfy it
# ---------------------------------------------------------------------------

_INJECT_DEFAULT = (
    "import argparse\n"
    "def main():\n"
    "    ap = argparse.ArgumentParser()\n"
    '    ap.add_argument("--missions-dir", default="A:/Games/Carver5-feasibility/data/missions")\n'
)
_INJECT_CONST = (
    "from pathlib import Path\n"
    'CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")\n'
)
_INJECT_OTHER_WRONG_TREE = (
    "from pathlib import Path\n"
    'MISSIONS = Path("D:/somewhere/else/entirely/data/missions")\n'
)
_COMMENT_ONLY = (
    "# historically this pointed at A:/Games/Carver5-feasibility/data/missions\n"
    "# and CARVER5_MISSIONS was the constant name\n"
    "import argparse\n"
    "def main():\n"
    "    ap = argparse.ArgumentParser()\n"
    '    ap.add_argument("--missions-dir", default=None)\n'
)
_DOCSTRING_ONLY = (
    '"""Reads stock maps.\n\n'
    'Historically A:/Games/Carver5-feasibility/data/missions.\n"""\n'
    "import argparse\n"
    "def main():\n"
    "    ap = argparse.ArgumentParser()\n"
    '    ap.add_argument("--missions-dir", default=None)\n'
)
_CLEAN = (
    "import argparse\n"
    "def main():\n"
    "    ap = argparse.ArgumentParser()\n"
    '    ap.add_argument("--missions-dir", default=None)\n'
    '    ap.add_argument("--out", default="tests/terrain/beautify")\n'
)

_CASES = [
    ("argparse default", _INJECT_DEFAULT, True),
    ("module constant", _INJECT_CONST, True),
    ("a DIFFERENT wrong tree", _INJECT_OTHER_WRONG_TREE, True),
    ("comment-only mention", _COMMENT_ONLY, False),
    ("docstring-only mention", _DOCSTRING_ONLY, False),
    ("clean file", _CLEAN, False),
]


def selftest() -> int:
    fails = []
    with tempfile.TemporaryDirectory() as td:
        for name, src, should_fire in _CASES:
            p = Path(td) / (re.sub(r"\W+", "_", name) + ".py")
            p.write_text(src, encoding="utf-8", newline="\n")
            fired = bool(scan_file(p))
            mark = "OK " if fired == should_fire else "BAD"
            print(f"  [{mark}] {name:<26} fires={fired} expected={should_fire}")
            if fired != should_fire:
                fails.append(name)

    # The resolver must REFUSE a wrong dir rather than return it.
    try:
        sm.resolve_missions_dir(explicit=Path(td) if False else Path(ROOT),
                                missions=["mc2_17"], quiet=True)
        print("  [BAD] resolver accepted a non-mission directory")
        fails.append("resolver refusal")
    except sm.StockDataError:
        print("  [OK ] resolver refuses an unverifiable --missions-dir")

    # ... but must return it when --allow-nonstock is explicit.
    try:
        r = sm.resolve_missions_dir(explicit=Path(ROOT), missions=["mc2_17"],
                                    allow_nonstock=True, quiet=True)
        ok = (not r.verified)
        print(f"  [{'OK ' if ok else 'BAD'}] --allow-nonstock returns "
              f"verified={r.verified} (want False)")
        if not ok:
            fails.append("allow-nonstock flagging")
    except sm.StockDataError:
        print("  [BAD] --allow-nonstock still refused")
        fails.append("allow-nonstock refusal")

    if fails:
        print(f"[stock-data] SELFTEST FAIL: {', '.join(fails)}")
        return 2
    print("[stock-data] SELFTEST PASS")
    return 0


# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="STOCK-DATA-TRUTH-1 gate")
    ap.add_argument("--no-data", action="store_true",
                    help="skip the machine-dependent data arm (contracts lane)")
    ap.add_argument("--write", action="store_true",
                    help="regenerate tools/mc2data/stock_missions.md5")
    ap.add_argument("--fst", help="mission.fst to generate from (with --write)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.write:
        return write_manifest(Path(args.fst) if args.fst else None)

    fails: list[str] = []
    notes: list[str] = []
    info: dict = {}

    static = static_scan()
    fails += static
    shape, minfo = manifest_shape()
    fails += shape
    info["manifest"] = minfo

    if not args.no_data:
        dfails, dnotes, dinfo = data_checks()
        fails += dfails
        notes += dnotes
        info["data"] = dinfo

    if args.json:
        print(json.dumps({"ok": not fails, "failures": fails,
                          "notes": notes, "info": info}, indent=2))
        return 0 if not fails else 2

    if not args.quiet:
        for n in notes:
            print(f"[stock-data] {n}")
    if fails:
        print(f"[stock-data] FAIL ({len(fails)} finding(s)):", file=sys.stderr)
        for f in fails:
            print(f"  {f}", file=sys.stderr)
        print("\n  Fix: drop the hard-coded path and use the verified resolver --\n"
              "    sys.path.insert(0, <repo>/tools/mc2data)\n"
              "    from stock_missions import add_missions_dir_arg, resolve_from_args\n"
              "  See tools/mc2data/stock_missions.py for why a hard-coded default "
              "is unsafe even when it happens to be right today.", file=sys.stderr)
        return 2
    if not args.quiet:
        print(f"[stock-data] PASS ({minfo.get('entries', 0)} manifest entries, "
              f"{minfo.get('top_level_paks', 0)} stock .pak)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
import stock_corpus as sc  # noqa: E402

SCAN_ROOTS = ("tools", "scripts")

# The files allowed to name stock-data directories in executable code -- they
# are the resolvers, naming them is their job. They do not get a free pass:
# check C verifies every candidate they name, and check A1b verifies they name
# no known-divergent tree.
ALLOWLIST = {
    "tools/mc2data/stock_missions.py",
    "tools/mc2data/stock_corpus.py",
    "scripts/check-stock-mission-data.py",
}

# Per-LITERAL exemptions (DATA-PROVENANCE-CLOSE-1). Deliberately not
# whole-file: a file listed here still fails on any OTHER hard-coded data path,
# so exempting one known string cannot quietly bless the next one. Each entry
# must carry a reason. These are the two classes this gate deliberately does
# not police (deploy-tree paths and external tool paths) plus one archived
# example script.
PATH_EXEMPTIONS: dict[str, list[tuple[str, str]]] = {
    "tools/examples/_prior_tree_export_impostor.py": [
        (r"A:\Games\mc2-opengl\mc2-win64-v0.3\data\tgl\128",
         "archived one-shot Blender example; a WRITE destination in a v0.3 "
         "deploy tree, not a stock-data read"),
    ],
    "tools/test_pak_append.py": [
        ("A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0/data/objects/object2.pak",
         "object2.pak is a LOOSE deploy-tree file, not a member of any .fst -- "
         "no manifest can cover it; the tests skipif it is absent"),
    ],
    "tools/test_pak_dump.py": [
        ("A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0/data/objects/object2.pak",
         "same loose deploy-tree pak as test_pak_append.py; skipif-guarded"),
    ],
    "scripts/bake_colormap_ktx2.py": [
        ("A:/Games/mc2-opengl/mc2-win64-v0.3/data/textures",
         "deploy-tree cook destination, sibling of the ktx.exe tool path -- "
         "the external-tool / deploy-lane class this gate excludes on purpose"),
    ],
    "scripts/deploy_payload.py": [
        ("A:/Games/mc2-opengl-src/mc2srcdata",
         "deploy-lane payload source probe. KNOWN OPEN ITEM, not a clean "
         "bill of health: it is the same probe-and-trust shape as the sites "
         "DATA-PROVENANCE-CLOSE-1 closed, but it feeds a deploy payload rather "
         "than an analysis verdict, and the deploy lane is out of scope here"),
    ],
}

# Trees PROVEN to differ from the shipped bytes. Must never appear as a
# resolver candidate. (Carver5-feasibility: differs on all 24 campaign maps,
# .pak and .fit, measured 2026-07-30.)
KNOWN_DIVERGENT = ("carver5-feasibility",)

# Directory names that identify an FST-backed data corpus. `missions` was the
# only one STOCK-DATA-TRUTH-1 policed; the rest are the corpora
# DATA-PROVENANCE-CLOSE-1 added (data/tgl <- tgl.fst, data/textures <-
# textures.fst incl. 64Overlays, data/objects <- misc.fst) plus the source-tree
# roots themselves.
CORPUS_DIR_NAMES = ("missions", "tgl", "textures", "64overlays", "objects",
                    "mc2srcdata", "mc2-srcdata")

# An absolute path (drive-letter, UNC, or POSIX root) that ends at, or descends
# through, one of those directory names as a FULL path component. Deliberately
# not anchored on any particular wrong tree: banning the literal
# `Carver5-feasibility` would let the identical defect return under a different
# wrong path; banning hard-coded absolute data paths cannot.
#
# Two deliberate narrowings, each of which killed real false positives when
# measured over the live tree (2026-07-30):
#   * a POSIX-rooted string needs >=2 leading components. `'/tgl/'` and
#     `'/64overlays/'` in tools/registry/build_index.py and
#     tools/overlay_upscale/ are SUBSTRING MATCHERS, not machine paths, and a
#     gate that cries wolf on them gets switched off.
#   * a string containing a character illegal in a Windows path (*?<>|") is a
#     glob or a help string, not a path -- e.g. the `'/*.tga  ->  <game>/data/
#     tgl/'` usage line in tools/mech_import/build_marauder.py.
_COMP = r"[^\n\\/]+"
_WINROOT = rf"(?:[A-Za-z]:[\\/]|\\\\)(?:{_COMP}[\\/])*"
_NIXROOT = rf"/(?:{_COMP}[\\/]){{2,}}"
_CORPUS_ALT = "|".join(re.escape(n) for n in CORPUS_DIR_NAMES)
DATA_PATH_RE = re.compile(
    rf"^(?:{_WINROOT}|{_NIXROOT})(?:{_CORPUS_ALT})(?:[\\/].*)?$",
    re.IGNORECASE | re.DOTALL)
_ILLEGAL_IN_PATH = set('*?<>|"')

# Directory/file CLI flags that must never carry a baked-in default -- the
# resolver has to run. `--missions` (a list of mission STEMS) is deliberately
# absent; a default there is fine and matching it flagged three innocent sites.
CORPUS_DIR_FLAGS = {
    "missions-dir", "mission-dir",
    "srcdata", "srcdata-root", "ase-root", "tga-root",
    "tgl-dir", "textures-dir", "overlays-dir", "objects-dir",
    "compbas", "effects",
}


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


def _exempt_literals(rel: str) -> set[str]:
    return {lit for lit, _why in PATH_EXEMPTIONS.get(rel, ())}


def scan_file(path: Path, rel_override: str | None = None) -> list[str]:
    """Return findings for one python file (empty == clean).

    `rel_override` exists for --selftest, which writes cases to a temp dir and
    still needs the exemption lookup keyed by the repo-relative name.
    """
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
    rel = rel_override if rel_override is not None else _rel(path)
    exempt = _exempt_literals(rel)

    # A1 -- absolute stock-data path literals in executable code.
    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
            continue
        if id(node) in docs:
            continue
        v = node.value.strip()
        if _ILLEGAL_IN_PATH & set(v):
            continue
        if not DATA_PATH_RE.match(v):
            continue
        if v in exempt:
            continue
        found.append(f"{rel}:{node.lineno}: hard-coded absolute stock-data "
                     f"path {v!r} in executable code")

    # A2 -- a corpus DIRECTORY flag with an argparse default other than None.
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
        matched = [n for n in names
                   if n.startswith("-")
                   and n.lstrip("-").replace("_", "-") in CORPUS_DIR_FLAGS]
        if not matched:
            continue
        for kw in node.keywords:
            if kw.arg == "default" and not (
                    isinstance(kw.value, ast.Constant) and kw.value.value is None):
                try:
                    shown = ast.unparse(kw.value)
                except Exception:
                    shown = "<expr>"
                found.append(f"{rel}:{node.lineno}: {matched[0]} has a "
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

    # A1b -- police the allowlisted resolver files.
    def _police(where: str, label: str, cands, empty_reason: str = "") -> None:
        for cand in cands:
            low = str(cand).replace("\\", "/").lower()
            for bad in KNOWN_DIVERGENT:
                if bad in low:
                    findings.append(
                        f"{where}: {label} contains {cand!r}, a tree PROVEN to "
                        f"differ from the shipped bytes")
        if not cands and len(empty_reason.strip()) < 40:
            # "Empty" must never mean "unexamined". A corpus that legitimately
            # has no fully-reproducing source tree says so, at length, in
            # `no_candidate_reason`; anything shorter reads as an oversight.
            findings.append(f"{where}: {label} is empty with no stated reason "
                            f"(set no_candidate_reason to the MEASUREMENT that "
                            f"makes it correct)")

    _police("tools/mc2data/stock_missions.py", "CANDIDATE_DIRS", sm.CANDIDATE_DIRS)
    for name, c in sorted(sc.CORPORA.items()):
        _police("tools/mc2data/stock_corpus.py",
                f"CORPORA[{name!r}].candidates", c.candidates,
                c.no_candidate_reason)
    _police("tools/mc2data/stock_corpus.py", "SRCDATA_ROOT_PROBES",
            sc.SRCDATA_ROOT_PROBES)

    # A1c -- an exemption whose literal no longer exists is DEAD and must be
    # removed. Otherwise the exemption list rots into a standing blind spot:
    # a file keeps its pass while the reason for it has gone.
    for rel, entries in sorted(PATH_EXEMPTIONS.items()):
        p = ROOT / rel
        if not p.is_file():
            findings.append(f"PATH_EXEMPTIONS names a missing file: {rel}")
            continue
        src = p.read_text(encoding="utf-8-sig", errors="replace")
        for lit, why in entries:
            if not why.strip():
                findings.append(f"PATH_EXEMPTIONS[{rel}] entry {lit!r} has no reason")
            if lit not in src:
                findings.append(
                    f"PATH_EXEMPTIONS[{rel}] exempts {lit!r}, which no longer "
                    f"appears in the file -- delete the stale exemption")
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


def corpus_manifest_shape() -> tuple[list[str], dict]:
    """B2 -- `tools/mc2data/stock_data.md5` exists, parses, and covers every
    registered corpus. A corpus with no members means the manifest predates it
    and every resolve() for it would refuse."""
    problems: list[str] = []
    info: dict = {"corpora": {}}
    if not sc.MANIFEST_PATH.is_file():
        return ([f"corpus manifest missing: {_rel(sc.MANIFEST_PATH)}; regenerate "
                 f"with: py -3 scripts/check-stock-mission-data.py --write-corpora"],
                info)
    hdr = sc.corpus_manifest_header()
    info["header"] = hdr
    for key in ("source_lane", "generated_by", "entries"):
        if key not in hdr:
            problems.append(f"corpus manifest header is missing `# {key}:`")
    total = 0
    for name in sorted(sc.CORPORA):
        c = sc.CORPORA[name]
        try:
            man = sc.load_corpus_manifest(c)
        except sc.StockDataError as e:
            problems.append(str(e))
            continue
        total += len(man)
        info["corpora"][name] = len(man)
        if f"{name}_fst_md5" not in hdr:
            problems.append(f"corpus manifest header is missing "
                            f"`# {name}_fst_md5:` (provenance of {c.fst})")
        for k, v in man.items():
            if len(v) != 32 or not re.fullmatch(r"[0-9a-f]{32}", v):
                problems.append(f"corpus manifest entry {name}/{k} has a "
                                f"non-md5 digest {v!r}")
                break
    info["entries"] = total
    if total < 1000:
        problems.append(f"corpus manifest has only {total} entries -- looks "
                        f"truncated (measured 2026-07-30: 5649)")
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

    cfails, cnotes, cinfo = corpus_data_checks()
    fails += cfails
    notes += cnotes
    info["corpora"] = cinfo
    return fails, notes, info


def corpus_data_checks() -> tuple[list[str], list[str], dict]:
    """C2 -- the same two arms as check C, for tgl/textures/objects.

    Every AUTOMATIC candidate present on this machine must reproduce its
    corpus; every deploy lane's archive must reproduce the committed digests.
    A lane that ever diverges FAILS here rather than being silently preferred.

    Note what is deliberately NOT asserted: `SRCDATA_ROOT_PROBES` entries are
    working checkouts and are NOT required to match in full -- `badpilots.csv`
    differs from `misc.fst` in every source tree on this machine. They are
    verified per name at the moment of use, which is the only claim that is
    both true and load-bearing.
    """
    fails: list[str] = []
    notes: list[str] = []
    info: dict = {"candidates": {}, "lanes": {}}
    if not sc.MANIFEST_PATH.is_file():
        return fails, ["corpus manifest absent -- corpus data arm skipped"], info

    for name in sorted(sc.CORPORA):
        c = sc.CORPORA[name]
        try:
            man = sc.load_corpus_manifest(c)
        except sc.StockDataError as e:
            fails.append(str(e))
            continue
        if not c.candidates:
            notes.append(f"[{name}] no automatic candidate by design: "
                         f"{c.no_candidate_reason.split('.')[0]}.")
        for cand in c.candidates:
            d = Path(cand)
            if not d.is_dir():
                notes.append(f"[{name}] candidate absent on this machine: {cand}")
                info["candidates"][f"{name}:{cand}"] = "absent"
                continue
            ok, n, bad, eo, un = sc.verify_dir(c, d, None, man)
            info["candidates"][f"{name}:{cand}"] = {
                "ok": ok, "checked": n, "mismatches": len(bad),
                "engine_only": eo, "unshipped": un}
            if ok:
                notes.append(f"[{name}] candidate VERIFIED ({n} files, {eo} "
                             f"engine-only, {un} unshipped): {cand}")
            else:
                fails.append(f"[{name}] candidate {cand} disagrees with the "
                             f"shipped bytes on {len(bad)} file(s), e.g. {bad[0]}")

        for r in sm.CANDIDATE_FST_ROOTS:
            fst = Path(r) / c.fst
            if not fst.is_file():
                info["lanes"][f"{name}:{r}"] = "absent"
                continue
            try:
                got = sm.fst_digests(fst, c.prefix)
            except sm.StockDataError as e:
                fails.append(f"{fst}: {e}")
                continue
            diff = sorted(k for k in set(got) | set(man) if got.get(k) != man.get(k))
            info["lanes"][f"{name}:{r}"] = {"members": len(got), "diffs": len(diff)}
            if diff:
                fails.append(f"deployed {fst} disagrees with the committed "
                             f"manifest on {len(diff)} member(s), e.g. {diff[0]} "
                             f"-- the lanes have DIVERGED; re-run with "
                             f"--write-corpora and re-verify")
            else:
                notes.append(f"[{name}] lane VERIFIED ({len(got)} members): {fst}")
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


def write_corpus_manifest(lane: Path | None) -> int:
    """Regenerate `tools/mc2data/stock_data.md5` from a deploy lane's archives.

    One file, keyed by FULL FST member path, covering every corpus in
    `stock_corpus.CORPORA`. Mission data keeps its own manifest on purpose --
    duplicating those digests here would create a SECOND authority for the same
    bytes, which is the failure this whole arc exists to prevent.
    """
    roots = [lane] if lane else [Path(r) for r in sm.CANDIDATE_FST_ROOTS]
    chosen: Path | None = None
    for r in roots:
        if all((r / c.fst).is_file() for c in sc.CORPORA.values()):
            chosen = r
            break
    if chosen is None:
        print("[stock-data] ERROR no deploy lane carries every archive "
              f"({', '.join(sorted({c.fst for c in sc.CORPORA.values()}))}); "
              "pass --lane PATH", file=sys.stderr)
        return 1

    lines = [
        "# DATA-PROVENANCE-CLOSE-1 -- digest of the stock data the engine ships,",
        "# for every FST-backed corpus EXCEPT missions (see stock_missions.md5).",
        "#",
        "# GENERATED FILE. Do not hand-edit.",
        "# Regenerate: py -3 scripts/check-stock-mission-data.py --write-corpora",
        "#",
        "# Authority: the deployed archives. mclib/file.cpp resolves",
        "#   active mod overlay -> loose file on disk -> FastFile(.fst),",
        "# and for these corpora the first two rungs miss in every lane (no lane",
        "# carries a loose data/objects/compbas.csv at all), so the bytes below",
        "# are definitionally what the engine loads. A source tree is usable only",
        "# by matching this file, per name, for the names actually read.",
        "#",
        f"# source_lane: {str(chosen).replace(chr(92), '/')}",
        "# generated_by: scripts/check-stock-mission-data.py --write-corpora",
    ]

    digests: dict[str, str] = {}
    per: list[str] = []
    for name in sorted(sc.CORPORA):
        c = sc.CORPORA[name]
        fst = chosen / c.fst
        d = sm.fst_digests(fst, c.prefix)
        for k, v in d.items():
            digests[(c.prefix + k).lower()] = v
        fst_md5 = sm._md5(fst)
        agree, disagree = [], []
        for r in sm.CANDIDATE_FST_ROOTS:
            other = Path(r) / c.fst
            if not other.is_file() or other == fst:
                continue
            (agree if sm._md5(other) == fst_md5 else disagree).append(str(other))
        lines.append(f"# {name}_fst: {str(fst).replace(chr(92), '/')}")
        lines.append(f"# {name}_fst_md5: {fst_md5}")
        lines.append(f"# {name}_entries: {len(d)}")
        lines.append(f"# {name}_byte_identical_lanes: {len(agree)}")
        lines.append(f"# {name}_divergent_lanes: {len(disagree)}")
        for other in sorted(disagree):
            lines.append(f"# LANE_DIVERGES: {other.replace(chr(92), '/')}")
        per.append(f"{name}={len(d)}")

    lines.append(f"# entries: {len(digests)}")
    lines.append("#")
    lines.append("# format: <md5>  <full FST member path>")
    for k in sorted(digests):
        lines.append(f"{digests[k]}  {k}")

    sc.MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    sc.MANIFEST_PATH.write_text("\n".join(lines) + "\n",
                                encoding="utf-8", newline="\n")
    print(f"[stock-data] wrote {_rel(sc.MANIFEST_PATH)}: {len(digests)} entries "
          f"({', '.join(per)}) from {chosen}")
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

_INJECT_TGL = (
    "from pathlib import Path\n"
    'ASE_ROOT = Path("A:/Games/mc2-srcdata/tgl")\n'
)
_INJECT_OVERLAYS = (
    "from pathlib import Path\n"
    'STOCK_OVERLAYS = Path("A:/Games/mc2-srcdata/textures/64Overlays")\n'
)
_INJECT_OBJECTS = (
    "CANDIDATES = [\n"
    '    "A:/Games/mc2-srcdata/objects/compbas.csv",\n'
    '    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/compbas.csv",\n'
    "]\n"
)
_INJECT_ASE_ROOT_DEFAULT = (
    "import argparse\n"
    "def main():\n"
    "    ap = argparse.ArgumentParser()\n"
    '    ap.add_argument("--ase-root", default="A:/Games/mc2-srcdata/tgl")\n'
)
_INJECT_SRCDATA_DEFAULT = (
    "import argparse\n"
    "def main():\n"
    "    ap = argparse.ArgumentParser()\n"
    "    ap.add_argument('--srcdata-root', default=SOME_CONSTANT)\n"
    "SOME_CONSTANT = 1\n"
)
# NOT paths: a substring classifier and a usage line. Both were live false
# positives before the regex was narrowed (tools/registry/build_index.py,
# tools/overlay_upscale/overlay_tile_upscale.py, tools/mech_import/
# build_marauder.py). A gate that fails on these gets turned off.
_SUBSTRING_MATCHER = (
    "def classify(p):\n"
    '    if "/tgl/" in p: return "tgl"\n'
    '    if "/64overlays/" in p: return "overlay"\n'
    '    if "/data/textures/" in p: return "tex"\n'
    '    return "other"\n'
)
_USAGE_LINE = (
    "def usage():\n"
    '    print("  /*.tga  ->  <game>/data/tgl/  AND  <game>/data/tgl/<size>/")\n'
)

_CASES = [
    ("argparse default", _INJECT_DEFAULT, True),
    ("module constant", _INJECT_CONST, True),
    ("a DIFFERENT wrong tree", _INJECT_OTHER_WRONG_TREE, True),
    ("tgl source tree", _INJECT_TGL, True),
    ("textures/64Overlays tree", _INJECT_OVERLAYS, True),
    ("objects csv candidates", _INJECT_OBJECTS, True),
    ("--ase-root default", _INJECT_ASE_ROOT_DEFAULT, True),
    ("--srcdata-root non-None default", _INJECT_SRCDATA_DEFAULT, True),
    ("comment-only mention", _COMMENT_ONLY, False),
    ("docstring-only mention", _DOCSTRING_ONLY, False),
    ("substring classifier", _SUBSTRING_MATCHER, False),
    ("usage/help line", _USAGE_LINE, False),
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

    # Same two properties for the corpus resolver.
    for cname in sorted(sc.CORPORA):
        try:
            sc.resolve(cname, explicit=Path(ROOT), names=["compbas.csv"]
                       if cname == "objects" else None, quiet=True)
            print(f"  [BAD] {cname} resolver accepted the repo root")
            fails.append(f"{cname} resolver refusal")
        except sc.StockDataError:
            print(f"  [OK ] {cname} resolver refuses an unverifiable dir")
    try:
        r = sc.resolve("objects", explicit=Path(ROOT), names=["compbas.csv"],
                       allow_nonstock=True, quiet=True)
        ok = not r.verified
        print(f"  [{'OK ' if ok else 'BAD'}] corpus --allow-nonstock returns "
              f"verified={r.verified} (want False)")
        if not ok:
            fails.append("corpus allow-nonstock flagging")
    except sc.StockDataError:
        print("  [BAD] corpus --allow-nonstock still refused")
        fails.append("corpus allow-nonstock refusal")

    # A requested name that is not a shipped member must be a MISMATCH, not a
    # silent pass -- otherwise "verified" would mean "verified nothing".
    try:
        sc.resolve("objects", names=["not_a_stock_file.csv"], quiet=True)
        print("  [BAD] corpus resolver accepted a non-shipped member name")
        fails.append("corpus unknown-name")
    except sc.StockDataError:
        print("  [OK ] corpus resolver refuses a non-shipped member name")

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
    ap.add_argument("--write-corpora", action="store_true",
                    help="regenerate tools/mc2data/stock_data.md5 "
                         "(tgl/textures/objects)")
    ap.add_argument("--fst", help="mission.fst to generate from (with --write)")
    ap.add_argument("--lane", help="deploy lane to generate from "
                                   "(with --write-corpora)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.write:
        return write_manifest(Path(args.fst) if args.fst else None)
    if args.write_corpora:
        return write_corpus_manifest(Path(args.lane) if args.lane else None)

    fails: list[str] = []
    notes: list[str] = []
    info: dict = {}

    static = static_scan()
    fails += static
    shape, minfo = manifest_shape()
    fails += shape
    info["manifest"] = minfo
    cshape, cinfo = corpus_manifest_shape()
    fails += cshape
    info["corpus_manifest"] = cinfo

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
        print(f"[stock-data] PASS ({minfo.get('entries', 0)} mission-manifest "
              f"entries, {minfo.get('top_level_paks', 0)} stock .pak; "
              f"{cinfo.get('entries', 0)} corpus-manifest entries "
              f"{cinfo.get('corpora', {})})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

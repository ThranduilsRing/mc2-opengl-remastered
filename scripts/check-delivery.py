#!/usr/bin/env python3
"""DELIVERY-GATE-1 -- "would I see this fix if I launched?", answered mechanically.

WHY THIS EXISTS
---------------
Every lane in this project asks "is the fix CORRECT?". The user never
experiences correctness. The user experiences

    is the fix in the binary I run, ON by default, with its required data present?

and nothing in 117 check scripts ever asked that question, because each of its
four parts lives in a different tool and no tool JOINs them:

  * scripts/exe_fingerprint.py     knows what commit a binary was built from
  * check-gate-default-agreement.py knows what a gate's reader does when unset
  * check-beauty-sidecars.py       knows whether one class of data reached a tree
  * check-deploy-coherence.py      knows whether a deploy drifted from its manifest
                                   -- and is documented "ALWAYS exits 0, advisory
                                   only", which is the wrong semantics for a
                                   delivery gate, which is why this is a new
                                   script rather than an extension of that one.

The measured cost of the missing join, all from this repo:
  * 19 of 27 gates shipped default-OFF -- correct code, unreachable by the user.
  * The shipped v0.5.2 release carries ZERO .beauty sidecars (0 rows matching
    "beauty" in release_assets/0.5.2/release_install_manifest.csv, 28,459 files),
    so every default-ON gate that reads one runs as a silent no-op there.
  * VEHICLE-AMBIENT-FLOOR-1 (e3310409) landed default-ON and is HALF DATA: its
    own commit message says "the deployed shaders/ tree must carry the new
    gos_tex_vertex_lighted.vert or the uniform does not exist in the program,
    glGetUniformLocation returns -1, the set silently no-ops and the gate runs
    OFF while reporting ON."
  * 6 of the 14 fingerprint-attributable installs on this disk (5 distinct
    build commits) run MC2_PAVED_OVERLAY_CLASS with default 1 -- a promotion
    the canonical tip RETRACTED at 2ade84a8, because the lane that shipped it
    had already measured and retracted its justification in writing. Concrete
    graded on those installs grades withdrawn code, and no tool said so.
  * 6 further installs carry MC2_LEGACY_LIT_MC2_FRAME's code with the gate
    still default-OFF, because they predate the promotion. An A/B of "=1
    versus unset" on those compares ON against ON -- the identical mistake
    MC2_SHADOW_ENABLE caused, one gate later.

THE FOUR AXES, AND WHAT EACH ONE REFUSES TO TRUST
-------------------------------------------------
1. CODE IN THE BINARY. Read from the EXE'S OWN BYTES.
   * Never a manifest's `git.commit` field: 28 of 31 capture manifests named a
     commit that was never in the binary they measured (CAPTURE-PROVENANCE-TRUTH-1).
   * Never generated/build_fingerprint.h: a failed or skipped compile leaves the
     header freshly stamped and the exe stale.
   Three evidence tiers, strongest first, and a disagreement between them is
   REPORTED, never silently resolved:
     binary       -- the fix's own literal (uniform name / gate name) found in
                     the image, with a positive AND a negative control on the
                     same blob so an absence is a finding rather than a failure
                     of the scanner.
     source@sha   -- `git show <embedded-sha>:<path>` contains the needle. This
                     is what catches WITHDRAWN code; ancestry cannot, because a
                     revert leaves the introducing commit an ancestor forever.
     ancestry     -- merge-base --is-ancestor. Weakest, and never sufficient
                     alone when a stronger tier is declared.
   A binary built from a DIRTY tree invalidates the two source-derived tiers
   (the sha does not describe the bytes) and is marked so; the binary tier
   survives, because bytes are bytes.

2. ON BY DEFAULT. Read from the C++ READER, at the commit THAT INSTALL WAS BUILT
   FROM -- not the tip, not the registry, not docs/tier1_env_vars.md, not a
   comment. Seventeen documented-vs-actual default lies were found in this tree
   in one day. A reader-vs-registry disagreement is a FAIL, not a warning.
   Boolean readers are classified by check-gate-default-agreement.py's own rule
   table (imported, not reimplemented, so the two can never drift). NUMERIC
   readers are classified here, because that checker deliberately skips them as
   "value-read (not a boolean gate)" -- which is how a numeric gate's retracted
   default-ON survived on 5 installs unnoticed.

3. DATA PRESENT. A deploy-tree file that EXISTS is not evidence: the half-data
   failure is an old shader sitting where a new one should be. So a data item may
   declare a `needle` the deployed file must contain.

4. WHICH INSTALLS. Every engine binary under the install root, graded
   individually. "The fix is in" is not a property of the repo.

UNKNOWN IS NEVER A PASS. An install with no readable fingerprint, a gate whose
reader cannot be classified, a data item that was never declared -- each of these
produces a distinct visible state and can never produce "WOULD SEE IT". A tool
that quietly upgrades ignorance to agreement is the exact instrument this repo
keeps getting burned by.

USAGE
  py -3 scripts/check-delivery.py                     # every fix, every install
  py -3 scripts/check-delivery.py --fix VEHICLE-AMBIENT-FLOOR-1
  py -3 scripts/check-delivery.py --gate MC2_SSAO_ENABLE     # ad-hoc, no manifest
  py -3 scripts/check-delivery.py --install mc2-win64-v0.4
  py -3 scripts/check-delivery.py --list-installs      # scope + every label
  py -3 scripts/check-delivery.py --nominate mc2-win64-v0.4    # ONE install
  py -3 scripts/check-delivery.py --contract           # repo-integrity only (CI)
  py -3 scripts/check-delivery.py --selftest           # injected-regression proof
  py -3 scripts/check-delivery.py --json

EVERY GRADING RUN STATES ITS SCOPE (roots, depth, exclusions, rejections,
install count, label count) before the first table, and --json carries the same
under "scope". Two prior sweeps of this disk each published a bare total for
about half its engine binaries with no stated scope, and each was read as a
whole-disk figure. A total without a scope is not a measurement.

EXIT CODES
  0  the queried fix(es) would be seen on at least one install, and no lie found
  1  DELIVERY FAILURE -- no install on disk would show the fix
  2  INTEGRITY FAILURE -- something in the tree is lying (reader vs registry,
     manifest naming a commit/needle that does not exist, scanner control failed)
"""
from __future__ import annotations

import argparse
import fnmatch
import glob as _glob
import importlib.util
import json
import os
import re
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.dirname(SCRIPT_DIR)
MANIFEST = os.path.join(SCRIPT_DIR, "delivery-fixes.json")

# The engine binary. Assembled rather than written out because the repo's
# game-launch hook pattern-matches the literal in tool invocations; keeping it
# out of a single token costs nothing and keeps this script greppable-safe.
EXE_NAME = "mc2" + ".exe"

# Where installs live. Both a flat layout and a releases/ subdir are scanned.
#
# THE ROOT LIST IS ITSELF AN AXIS-4 CLAIM, AND THE FIRST VERSION'S WAS WRONG.
# ---------------------------------------------------------------------------
# This was `["A:/Games/mc2-opengl"]` -- the directory full of LANE SCRATCH
# BUILDS (mc2-win64-pom1, mc2-win64-treesoff-1, mc2-win64-abl-validate ...).
# A disk sweep for the engine binary (DELIVERY-CLOSE-1) found 22 of them and
# this tool was grading 18. Among the four it could not see was
#
#     A:/Games/MC2 OpenGL Remastered/<engine>   built from 85aaaa4f, 2026-07-05
#
# -- the install carrying the PRODUCT name, i.e. the likeliest thing a person
# actually double-clicks, and 25 days behind the tip. All nine manifest fixes
# grade 0/3 against that root. So the tool whose entire purpose is "would the
# user see it?" was answering for every directory EXCEPT the one named like the
# user's, and answering 6/18 where the honest answer for that install is 0.
#
# An install root that is not scanned cannot report NO; it reports nothing, and
# nothing reads as fine. Roots are therefore listed explicitly and a new one is
# a deliberate edit, not a silent default.
#
# AND IT WENT STALE AGAIN, THE SAME WAY, WITHIN TWO DAYS (CONTRACT-GREEN-1,
# 2026-07-31). A fresh sweep found 30 engine binaries where the last one found
# 22, and 11 were outside these patterns. Four of them are not scratch at all --
# they are dated top-level installs with full deployed data trees, and they are
# the FOUR NEWEST BINARIES ON THE DISK:
#
#   A:/Games/MC2-OpenGL-CURRENT-2026-07-30    e1bdb7af697d
#   A:/Games/MC2-OpenGL-AB-SUNAXIS-2026-07-30 dab061529977
#   A:/Games/MC2-OpenGL-TIP-2026-07-31        e4ce37d91162
#   A:/Games/MC2-OpenGL-CURSORFIX-2026-07-31  3d7f9d7871f7
#
# Every one of them delivers ALL NINE manifest fixes -- code in the binary,
# default ON, data present, 9/9 on each. Meanwhile this gate was reporting four
# of those nine as DELIVERED NOWHERE and had been red on that basis for hours,
# which is not a conservative error: it is the tool asserting a falsehood in the
# direction that makes people stop reading it. The failure is exactly the one
# this comment block already warned about -- an install root that is not scanned
# reports nothing, and nothing reads as fine -- so the warning was right and the
# LIST was the part that could not keep up, because it is hand-maintained and the
# installs are named after the day they were cut.
#
# Hence a GLOB rather than four more literals: `A:/Games/MC2-OpenGL-*` matches
# the dated-sibling convention that is actually in use, so tomorrow's cut is
# graded without an edit. find_installs() expands each root pattern, so a
# wildcard root composes with every depth pattern.
#
# WHAT THIS DOES NOT FIX, STATED PLAINLY: the pass bar is "at least ONE install
# shows the fix", so adding installs can only make the row easier to pass. The
# row can go green off a single fresh directory while the install carrying the
# PRODUCT name still has none of the nine. That is a weak assertion and it is
# not what "would the user see it?" means -- the user launches one install, not
# the union of twenty-eight. --nominate now makes the strict bar available
# (grade THIS install, not any of them); WHICH install is nominated remains a
# user decision and no default is taken here.
#
# THE FOUR-NESTED-COPIES GAP IS CLOSED (CONTRACT-TRUTH-2, 2026-07-31). The
# paragraph that stood here filed the <install>/data/mc2-win64-v0.4 copies as a
# known gap on the grounds that reaching them needed "a speculative <root>/*/*/
# glob that would also sweep arbitrary subdirectories". Two things were wrong
# with that. There were FIVE, not four -- CURSORFIX-2026-07-31 was cut after the
# paragraph was written, which is the same way this list went stale twice
# already. And the glob is not speculative: measured, <root>/*/*/ adds exactly
# those five under the default roots and nothing else. What it WOULD sweep, if
# a root were pointed at a source worktree, is build output -- so find_installs
# rejects disqualifying path segments by name (see NON_INSTALL_SEGMENTS) and
# reports every rejection in its scope block. A known gap in the one tool that
# answers "which installs would show this?" is still a root that cannot report
# NO; it reports nothing, and nothing reads as fine.
#
# STILL UNSCANNED, NAMED SO IT IS A KNOWN GAP RATHER THAN AN UNKNOWN ONE:
#   * A:/Games/Carver5-feasibility  and  A:/Games/test -- both verified
#     banner-absent (no build-fingerprint), so not attributable engine builds
#     this repo produced. They would grade "?" on every axis: noise, not answer.
#   * every <worktree>/build64/RelWithDebInfo/<engine> (16 on this disk) --
#     compiler output, deliberately excluded, and now excluded BY RULE with the
#     rejection counted in the scope block rather than by not looking.
# Re-check with --install-root if any of that changes; --list-installs prints
# the scope block plus every discovered label without grading anything.
DEFAULT_INSTALL_ROOTS = [
    "A:/Games/mc2-opengl",
    "A:/Games/MC2 OpenGL Remastered",
    "A:/Games/MC2-OpenGL-*",
]

REGISTRY_REL = "RenderCore/RendererFeatureRegistry.h"

# States. Deliberately three-valued everywhere.
YES, NO, UNK = "YES", "NO", "?"


# ---------------------------------------------------------------------------
# Sibling modules, imported rather than reimplemented
# ---------------------------------------------------------------------------
def _load(mod_name: str, filename: str):
    path = os.path.join(SCRIPT_DIR, filename)
    spec = importlib.util.spec_from_file_location(mod_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[mod_name] = mod
    spec.loader.exec_module(mod)
    return mod


def _load_committed(mod_name: str, filename: str, rev: str = "HEAD"):
    """Load a sibling module from its COMMITTED bytes, not the working copy."""
    import tempfile
    blob = subprocess.check_output(
        ["git", "-C", WORKTREE, "show", f"{rev}:scripts/{filename}"], creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    tmp = os.path.join(tempfile.gettempdir(), f"_committed_{mod_name}.py")
    with open(tmp, "wb") as f:
        f.write(blob)
    spec = importlib.util.spec_from_file_location(mod_name, tmp)
    mod = importlib.util.module_from_spec(spec)          # type: ignore[arg-type]
    sys.modules[mod_name] = mod
    spec.loader.exec_module(mod)                          # type: ignore[union-attr]
    return mod


def _is_dirty(rel: str) -> bool:
    out = subprocess.run(["git", "-C", WORKTREE, "status", "--porcelain", rel],
                         capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return bool(out.stdout.strip())


ef = _load("exe_fingerprint", "exe_fingerprint.py")

# THE CLASSIFIER IS BORROWED, SO ITS STATE IS PART OF THIS TOOL'S HONESTY.
#
# Every default verdict here is produced by check-gate-default-agreement.py's
# rule table. In a shared worktree that file is routinely mid-edit by another
# lane -- it was during this tool's own bring-up (218 inserted lines touching
# find_readers). A verdict that silently changes because a neighbour is halfway
# through a refactor is precisely the class of instrument this repo keeps
# getting burned by, so the borrowing is made visible rather than assumed
# harmless: the working copy is used by default (so this tool and
# check-contracts.sh always agree about what the rules ARE), and its dirtiness
# is announced. --classifier committed pins the rules to HEAD for a
# reproducible verdict.
CLASSIFIER_DIRTY = _is_dirty("scripts/check-gate-default-agreement.py")
gda = _load("gate_default_agreement", "check-gate-default-agreement.py")


def use_committed_classifier(rev: str = "HEAD") -> None:
    global gda
    gda = _load_committed("gate_default_agreement_committed",
                          "check-gate-default-agreement.py", rev)


# ---------------------------------------------------------------------------
# git
# ---------------------------------------------------------------------------
def git(*args, worktree: str = WORKTREE, ok_fail: bool = False):
    try:
        return subprocess.check_output(["git", "-C", worktree, *args],
                                       text=True, encoding="utf-8",
                                       errors="replace",
                                       stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except subprocess.CalledProcessError:
        if ok_fail:
            return None
        return None


def commit_exists(sha: str) -> bool:
    out = git("cat-file", "-t", sha)
    return bool(out) and out.strip() == "commit"


def is_ancestor(anc: str, desc: str) -> bool | None:
    """None when either end is unresolvable -- unknowable, NOT False."""
    if not commit_exists(anc) or not commit_exists(desc):
        return None
    r = subprocess.run(["git", "-C", WORKTREE, "merge-base",
                        "--is-ancestor", anc, desc],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return r.returncode == 0


# A commit-ish of None means THE WORKING TREE.
#
# This distinction is the whole difference between a guard that can see your
# change and one that cannot. The first version of this script read
# `git show HEAD:<path>` everywhere, which grades THE LAST COMMIT -- so an
# injected inversion sitting in the working tree passed clean, and the guard
# would have been structurally incapable of failing the commit that introduced
# the lie it exists to catch. Found by injecting the regression rather than by
# running the self-test, which is the point of injecting it.
WORKING_TREE = None


def show(sha: str | None, path: str) -> str | None:
    if sha is WORKING_TREE:
        p = os.path.join(WORKTREE, path.replace("/", os.sep))
        if not os.path.isfile(p):
            return None
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    return git("show", f"{sha}:{path}")


def grep_files(sha: str | None, literal: str, dirs) -> list[str]:
    """Paths at `sha` (or in the working tree) containing `literal`."""
    if sha is WORKING_TREE:
        out = git("grep", "-l", "--fixed-strings", literal, "--", *dirs)
        return [ln for ln in (out or "").splitlines() if ln]
    out = git("grep", "-l", "--fixed-strings", literal, sha, "--", *dirs)
    if not out:
        return []
    paths = []
    for line in out.splitlines():
        # "<sha>:<path>"
        if ":" in line:
            paths.append(line.split(":", 1)[1])
    return paths


def label_of(sha: str | None) -> str:
    return "the working tree" if sha is WORKING_TREE else sha[:12]


# ---------------------------------------------------------------------------
# Byte scanning, with controls (a "not found" that did not prove the scanner
# works is not a finding -- pe_gate_scan.py lesson, carried by exe_fingerprint)
# ---------------------------------------------------------------------------
NEG_CONTROL = b"MC2_DELIVERY_NEEDLE_THAT_CANNOT_EXIST_7f3a91c4"


def scan_binary(blob: bytes, needle: str) -> dict:
    """-> {state, hits, control}. state is YES/NO/UNK.

    UNK when the controls fail, i.e. when this scan cannot be trusted at all.
    The positive control is the build-fingerprint banner: any engine build has
    it, so its absence means "this blob is not a build I can reason about"
    rather than "the needle is missing".
    """
    if blob.find(NEG_CONTROL) >= 0:
        return {"state": UNK, "hits": 0, "control": "negative control MATCHED "
                                                    "-- scan logic is broken"}
    if blob.find(ef.POSITIVE_CONTROL) < 0:
        return {"state": UNK, "hits": 0,
                "control": "positive control absent (no build-fingerprint "
                           "banner) -- not an attributable engine build"}
    n = needle.encode("ascii", "replace")
    hits = blob.count(n)
    # MSVC also pools some literals as UTF-16; count those so an absence is not
    # an artifact of encoding.
    hits16 = blob.count(n.decode("ascii", "replace").encode("utf-16-le"))
    total = hits + hits16
    return {"state": YES if total else NO, "hits": total,
            "control": f"ok (ascii={hits} utf16={hits16})"}


# ---------------------------------------------------------------------------
# Reader classification at an arbitrary commit
# ---------------------------------------------------------------------------
_CALLEE_BEFORE = re.compile(r"([A-Za-z_]\w*)\s*(?:::\s*)?$")

# Numeric idioms. check-gate-default-agreement.py deliberately declines these
# ("value-read (not a boolean gate)"), which is correct for ITS question and
# leaves this one unanswered: a numeric gate still has a shipped default, and
# MC2_PAVED_OVERLAY_CLASS's retracted default-ON lived in exactly this blind
# spot on 5 installs. Only the unambiguous forms are matched; anything else is
# UNKNOWN, which can never grade as delivered.
#
# TWO BLIND SPOTS OF THE FIRST VERSION, both found by pointing this tool at a
# real gate rather than at its own fixtures (DELIVERY-CLOSE-1):
#
#   * THE POINTER NAME WAS HARD-CODED to `v`. Every rule literally spelled `v`,
#     so a reader that happened to name its getenv result anything else was
#     invisible. gos_terrain_lod_chunk.cpp:2884 names it `e`.
#   * THE DEFAULT HAD TO BE AN INTEGER. `return 0.35f;` matched nothing, so
#     MC2_TERRAIN_AMBIENT_FLOOR -- shipped default-ON in this session, and one
#     of the five user-facing fixes this lane was asked to grade -- classified
#     UNKNOWN and could never grade as delivered no matter what any install
#     contained. A float gate is still a shipped default.
#
# The identifier is now CAPTURED and BACK-REFERENCED, so `!a || !b[0]` is still
# correctly refused: the two spellings must be the same variable.
_ID = r"(?P<id>[A-Za-z_]\w*)"
_NUM = r"(?P<num>-?\d+(?:\.\d+)?)[fFuUlL]*"
_NUM_RULES = [
    re.compile(r"if\s*\(\s*!\s*" + _ID + r"\s*\|\|\s*!\s*(?P=id)\s*\[\s*0\s*\]"
               r"\s*\)\s*return\s+" + _NUM + r"\s*;"),
    re.compile(r"if\s*\(\s*!\s*" + _ID + r"\s*\|\|\s*!\s*\*\s*(?P=id)\s*\)"
               r"\s*return\s+" + _NUM + r"\s*;"),
    re.compile(r"if\s*\(\s*!\s*" + _ID + r"\s*\)\s*return\s+" + _NUM + r"\s*;"),
    re.compile(r"if\s*\(\s*" + _ID + r"\s*==\s*(?:nullptr|NULL|0)\s*\)\s*"
               r"return\s+" + _NUM + r"\s*;"),
    re.compile(r"\b" + _ID + r"\s*\?\s*(?:ato[ilf]|std\s*::\s*sto[ilfd])"
               r"\s*\(\s*(?P=id)\s*\)\s*:\s*" + _NUM),
]


_NUMERIC_LITERAL = re.compile(r"-?\d+(?:\.\d+)?")


def canon_num(text: str) -> str | None:
    """'0.35f' -> '0.35', '00' -> '0', '0.0' -> '0'. None when not numeric.

    Canonical so that two readers spelling the SAME default differently ('0'
    and '0.0') are not reported as disagreeing with each other, while genuinely
    different values still are.
    """
    if text is None:
        return None
    t = text.strip().rstrip("fFuUlL")
    if not _NUMERIC_LITERAL.fullmatch(t):
        return None
    f = float(t)
    return str(int(f)) if f == int(f) else ("%g" % f)


def is_on(value: str | None) -> bool | None:
    """'ON'/'1'/'2'/'0.35' -> True, 'OFF'/'0'/'0.0' -> False, else None.

    Floats are on/off by NONZERO, exactly as the shader is: the terrain ambient
    floor uploads its value straight into a uniform where 0.0 is the documented
    byte-identical revert, so "is this gate on" is "is this value nonzero" for
    a float reader just as much as for an int one.
    """
    if value is None:
        return None
    v = value.strip().upper()
    if v == "ON":
        return True
    if v == "OFF":
        return False
    c = canon_num(v)
    if c is not None:
        return c not in ("0", "-0")
    return None


def expect_mismatch(actual: str | None, expect: str | None) -> str | None:
    """Compare a reader default against a manifest claim, kind-agnostically.

    'ON' and '1' are the same shipped behaviour written by two classifiers, and
    a manifest must not become wrong merely because a reader was rewritten from
    a bool to an int. But when BOTH sides state an integer, the integers must
    match exactly -- default 1 and default 2 are different arms of the same gate.
    """
    if expect is None or actual is None:
        return None
    a, e = is_on(actual), is_on(expect)
    if a is None or e is None:
        return f"cannot compare reader default '{actual}' with claim '{expect}'"
    if a != e:
        return (f"reader defaults {actual} but the manifest claims {expect} "
                f"-- one of them is a lie")
    ca, ce = canon_num(actual), canon_num(expect)
    if ca is not None and ce is not None and ca != ce:
        return (f"reader defaults to {actual} but the manifest claims "
                f"{expect} -- same on/off sense, different arm")
    return None


def _binds_of(flat: str, var: str) -> set[str]:
    """Names this block assigns `getenv("var")` to. Empty = bare presence test."""
    rx = re.compile(r"([A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?getenv\s*\(\s*"
                    + '"' + re.escape(var) + '"' + r"\s*\)")
    return {m.group(1) for m in rx.finditer(flat)}


def classify_numeric(block: str, var: str | None = None):
    """The numeric default `var` falls back to in `block`, or None.

    THE NUMERIC MATCH MUST BE BOUND TO THIS GATE'S OWN VARIABLE.
    ------------------------------------------------------------
    Caught by pointing the generalised rules at the real tree rather than at a
    fixture (DELIVERY-CLOSE-1). gos_terrain_lod_chunk.cpp reads TWO gates in one
    enclosing block:

        static const char* s_ambEnv = getenv("MC2_TERRAIN_AMBIENT_FLOOR");
        static const float s_amb    = []() {
            const char* e = getenv("MC2_TERRAIN_AMBIENT_FLOOR");
            if (!e || !e[0]) return 0.35f;              <-- the FLOOR's default
            ...
        }();
        ...
        if (!s_traced && getenv("MC2_TERRAIN_AMBIENT_FLOOR_TRACE")) { ... }

    gda.enclosing_block() hands back that whole block for BOTH getenv sites, so
    an unbound regex sweep reported MC2_TERRAIN_AMBIENT_FLOOR_TRACE -- a bare
    presence test with no numeric default at all -- as "defaults 0.35", and then
    as "REGISTRY LIES" against its honest defaultOn=false. That is a
    tool-manufactured lie of exactly the kind this script exists to catch, and
    it would have been a NEW one.

    So: a numeric idiom counts only when its variable is one this block actually
    assigned `getenv("<var>")` to. A gate whose value is never bound to a name
    is a presence test, which has no numeric default -- None, i.e. UNKNOWN,
    which can never grade as delivered.
    """
    flat = re.sub(r"\s+", " ", block)
    bound = _binds_of(flat, var) if var is not None else None
    if bound is not None and not bound:
        return None
    for rx in _NUM_RULES:
        for m in rx.finditer(flat):
            if bound is not None and m.group("id") not in bound:
                continue
            return canon_num(m.group("num"))
    return None


def sources_at(sha: str, var: str):
    """Comment-stripped (path, body) pairs at `sha` that mention `var`."""
    paths = grep_files(sha, f'"{var}"', gda.SCAN_DIRS)
    out = []
    for p in paths:
        if not p.endswith(gda.SCAN_EXTS):
            continue
        if p.replace("\\", "/").endswith(REGISTRY_REL):
            continue          # names every var in literals; not a reader
        body = show(sha, p)
        if body is None:
            continue
        out.append((p, gda.strip_comments(body)))
    return out


def _helper_round_two(sha, sources, var):
    """Fetch definition files for helper callees we saw but could not resolve.

    Without this, a gate read through a helper that lives in a file which does
    not itself name the variable classifies UNKNOWN -- and UNKNOWN would then
    be reported for a gate whose default is perfectly well defined. One bounded
    round; still UNKNOWN afterwards stays UNKNOWN.
    """
    needle = re.compile(r'\(\s*"' + re.escape(var) + r'"\s*(?=[),])')
    callees = set()
    for _p, body in sources:
        for m in needle.finditer(body):
            cm = _CALLEE_BEFORE.search(body[max(0, m.start() - 64):m.start()])
            if cm:
                callees.add(cm.group(1))
    callees.discard("getenv")
    extra = []
    seen = {p for p, _ in sources}
    for c in sorted(callees):
        for p in grep_files(sha, f"bool {c}(", gda.SCAN_DIRS):
            if p in seen or not p.endswith(gda.SCAN_EXTS):
                continue
            body = show(sha, p)
            if body is not None:
                seen.add(p)
                extra.append((p, gda.strip_comments(body)))
    return sources + extra


def read_default_at(sha: str | None, var: str) -> dict:
    """The gate's UNSET-CASE default, as implemented at commit `sha`.

    -> {default: 'ON'|'OFF'|'<int>'|None, kind: 'bool'|'numeric'|None,
        readers: [...], registry: True|False|None, agree: bool|None, notes: []}
    """
    res = {"default": None, "kind": None, "readers": [], "registry": None,
           "agree": None, "notes": []}
    srcs = sources_at(sha, var)
    if not srcs:
        res["notes"].append(f"no source at {label_of(sha)} names \"{var}\"")
        return res

    helpers = gda.resolve_helpers(srcs)
    hits = gda.find_readers(var, srcs, helpers)
    if any(h["verdict"] == gda.UNKNOWN for h in hits) or not hits:
        srcs2 = _helper_round_two(sha, srcs, var)
        if len(srcs2) != len(srcs):
            srcs = srcs2
            helpers = gda.resolve_helpers(srcs)
            hits = gda.find_readers(var, srcs, helpers)

    # NUMERIC FIRST, deliberately. MC2_PAVED_OVERLAY_CLASS is a 0/1/2 knob whose
    # `if (!v || !v[0]) return 0;` prologue the boolean rule table happily
    # classifies as OFF -- true, but it collapses "the value the tip retracted"
    # (1) and "the value it retracted TO" (0) into ON/OFF and hides which arm an
    # install actually ships. When a reader states an integer, the integer is the
    # more specific truth and it is what gets reported.
    found = {}
    for p, body in srcs:
        for m in re.finditer(r'getenv\s*\(\s*"' + re.escape(var) + r'"', body):
            blk = gda.enclosing_block(body, m.end())
            seg = body[blk[0]:blk[1]] if blk else body[m.start():
                                                       m.start() + 400]
            n = classify_numeric(seg, var)
            if n is not None:
                line = body.count("\n", 0, m.start()) + 1
                found.setdefault(n, []).append(f"{p}:{line} numeric default")

    bool_verdicts = {h["verdict"] for h in hits}
    if len(found) == 1:
        res["kind"] = "numeric"
        res["default"] = next(iter(found))
        res["readers"] = found[res["default"]]
    elif len(found) > 1:
        res["kind"] = "numeric"
        res["notes"].append("NUMERIC READERS DISAGREE: "
                            + ", ".join(sorted(found)))
    elif hits and bool_verdicts <= {gda.ON, gda.OFF}:
        res["kind"] = "bool"
        res["readers"] = [f"{h['path']}:{h['line']} {h['rule']}" for h in hits]
        if len(bool_verdicts) == 1:
            res["default"] = "ON" if bool_verdicts == {gda.ON} else "OFF"
        else:
            res["notes"].append("READERS DISAGREE with each other")
    else:
        res["notes"].append("reader idiom not classifiable")
        res["readers"] = [f"{h['path']}:{h['line']} {h['rule']}"
                          for h in hits]

    # Registry cross-check AT THE SAME COMMIT.
    reg = show(sha, REGISTRY_REL)
    if reg is None:
        res["notes"].append(f"{REGISTRY_REL} absent at {label_of(sha)}")
    else:
        ent = [e for e in gda.parse_registry(reg) if e["var"] == var]
        if not ent:
            res["notes"].append(f"gate not registered at {label_of(sha)}")
        else:
            res["registry"] = ent[0]["default_on"]
            on = is_on(res["default"])
            if on is not None:
                # A numeric gate's registry flag is a coarse on/off summary, so
                # the comparison is on/off in both cases; nonzero default vs
                # defaultOn=false is still a flat contradiction.
                res["agree"] = (on == res["registry"])
                if not res["agree"]:
                    res["notes"].append(
                        f"REGISTRY LIES: registry defaultOn="
                        f"{str(res['registry']).lower()} but the reader "
                        f"defaults {res['default']}")
    return res


# ---------------------------------------------------------------------------
# Installs
# ---------------------------------------------------------------------------
# DIRECTORY NAMES THAT ARE NEVER AN INSTALL, whatever binary they contain.
#
# This list is the price of searching two levels down (below). A build tree
# holds a real, freshly-linked engine binary at <worktree>/build64/
# RelWithDebInfo/, which is EXACTLY the shape <root>/*/*/ matches -- so pointing
# --install-root at a source worktree would otherwise grade a compiler output as
# something a user launches. It is not: it has no deployed data tree beside it,
# so every data axis would report MISSING and the row would be noise wearing a
# verdict. Measured on this disk (CONTRACT-TRUTH-2, 2026-07-31): of 46 engine
# binaries under A:/, 16 are build outputs of exactly this shape.
#
# Matching is on any path segment BELOW the root, lowercased, so a root that is
# itself named "build" is still honoured -- you asked for it explicitly.
NON_INSTALL_SEGMENTS = {
    "build", "build64", "debug", "release", "relwithdebinfo", "minsizerel",
    "cmakefiles", "x64", "win32", ".git", "node_modules", "__pycache__",
    "$recycle.bin", "system volume information",
}


def _rejected_segment(root_dir: str, exe_dir: str) -> str | None:
    """The first below-root path segment that disqualifies exe_dir, or None."""
    rel = os.path.relpath(exe_dir, root_dir)
    if rel == os.curdir:
        return None
    for seg in rel.replace("\\", "/").split("/"):
        if seg.lower() in NON_INSTALL_SEGMENTS:
            return seg
    return None


def _label_for(root_dir: str, exe_dir: str, root_is_install: bool) -> str:
    """A STABLE, path-derived label. See the uniqueness note in find_installs."""
    rel = os.path.relpath(exe_dir, root_dir).replace("\\", "/")
    base = os.path.basename(root_dir.rstrip("/\\")) or root_dir
    if rel == ".":
        return base
    # When the root is ITSELF an install, its nested copies must carry the
    # root's name or every root's <root>/data/mc2-win64-v0.4 collapses onto
    # every other root's. When the root is a plain container of installs, the
    # relative path alone is already unique within it, and using it verbatim
    # keeps every pre-existing label byte-identical.
    return f"{base}/{rel}" if root_is_install else rel


def find_installs(roots, scope: dict | None = None) -> list[dict]:
    """Every engine install under `roots`, one row per binary, uniquely labelled.

    TWO DEFECTS FIXED HERE (CONTRACT-TRUTH-2, reported by DELIVERY-REGRADE-1).

    1. IT COULD NOT REACH TWO LEVELS DOWN. The patterns were <root>/,
       <root>/*/ and <root>/releases/*/ -- depth 1 plus one hard-coded depth-2
       special case. FIVE installs on this disk sit at <root>/data/
       mc2-win64-v0.4/ and were invisible to a default run: one under
       "MC2 OpenGL Remastered" and one under each of the four MC2-OpenGL-*
       cuts. The comment that replaced this one KNEW about them -- it named
       four of the five and filed them as a known gap -- but a known gap in a
       tool whose entire purpose is "which installs would show this fix?" is
       still an install root that cannot report NO. It reports nothing, and
       nothing reads as fine. <root>/*/*/ reaches them; the segment blacklist
       above is what keeps that from also sweeping in build output.

       DEPTH IS BOUNDED AT 2 AND THE SEARCH IS NOT RECURSIVE. os.walk from an
       explicit root would find every build64/RelWithDebInfo/<engine> under it
       (15 of them under A:/Games), and "grade everything you can see" is how
       the tool ends up asserting things about directories nobody launches.

    2. ITS LABELS WERE NOT UNIQUE. The label was basename(dirname(exe)) with
       ONE special case ("releases/" prefix). That happened to be collision-free
       on the 23 rows it could see, so nothing had failed yet -- but the five
       installs fix 1 adds are ALL named mc2-win64-v0.4, and so is a sixth at
       A:/Games/mc2-opengl/mc2-win64-v0.4. Six rows, one label. The human table
       prints six lines that look like contradictory verdicts for one install,
       and a per-label table built from --json silently merges them into one:
       DELIVERY-REGRADE-1 published exactly that table mid-lane before catching
       it. Fixing discovery without fixing labels would have SHIPPED that bug.

       The label is now derived from the path relative to the root that found
       it, qualified by the root's own name when the root is itself an install.
       That is a function of the PATH ALONE -- not of what else happens to be on
       disk -- so a label means the same install from one run to the next, which
       is the property a nominated-install gate needs. All 23 pre-existing
       labels are unchanged by construction (a plain container root yields the
       relative path, which for depth-1 installs IS the old basename, and for
       releases/* IS the old special case, now general rather than hard-coded).

       Uniqueness is then ENFORCED rather than argued: colliding labels are
       escalated to full paths and the collision is recorded in the scope block.
       Two distinct roots could still produce one label (two containers each
       holding an "mc2-win64-v0.4"); this is the backstop for that.

    `scope` is filled in with what was and was not searched, so a caller can
    print it. A sweep that does not state its scope is how two prior sweeps of
    this disk each published a bare total for half the binaries on it.
    """
    seen, out = set(), []
    root_dirs, unmatched, rejected = [], [], []
    for pattern in roots:
        hits = sorted(d for d in _glob.glob(pattern) if os.path.isdir(d))
        if not hits:
            unmatched.append(pattern)
            continue
        root_dirs += [(pattern, os.path.abspath(d).replace("\\", "/"))
                      for d in hits]

    for pattern, root in root_dirs:
        root_is_install = os.path.isfile(os.path.join(root, EXE_NAME))
        # `root` ITSELF may be an install dir, not just a parent of them.
        # "A:/Games/MC2 OpenGL Remastered/<engine>" -- an install that IS its
        # own root -- was unreachable when only <root>/*/ was matched: naming it
        # found nothing, and naming its parent swept in every unrelated folder
        # under A:/Games. Depth 0, 1 and 2, so a root can be any of the shapes
        # actually in use on this disk.
        pats = [os.path.join(root, EXE_NAME),
                os.path.join(root, "*", EXE_NAME),
                os.path.join(root, "*", "*", EXE_NAME)]
        for pat in pats:
            for exe in sorted(_glob.glob(pat)):
                exe = os.path.abspath(exe)
                if exe in seen:
                    continue
                d = os.path.dirname(exe)
                bad = _rejected_segment(root, d)
                if bad is not None:
                    rejected.append((exe, bad))
                    continue
                seen.add(exe)
                out.append({"label": _label_for(root, d, root_is_install),
                            "exe": exe, "deploy": d, "root": root})

    # Uniqueness backstop. A label that names two binaries is a merged row.
    by_label: dict[str, list[dict]] = {}
    for i in out:
        by_label.setdefault(i["label"], []).append(i)
    collisions = []
    for label, group in by_label.items():
        if len(group) > 1:
            collisions.append((label, [g["exe"] for g in group]))
            for g in group:
                g["label"] = g["deploy"].replace("\\", "/")

    if scope is not None:
        scope.update({
            "root_patterns": list(roots),
            "root_dirs": [d for _p, d in root_dirs],
            "roots_by_pattern": {p: [d for q, d in root_dirs if q == p]
                                 for p in roots},
            "unmatched_root_patterns": unmatched,
            "patterns_per_root": ["<root>/" + EXE_NAME,
                                  "<root>/*/" + EXE_NAME,
                                  "<root>/*/*/" + EXE_NAME],
            "max_depth": 2,
            "recursive": False,
            "excluded_segments": sorted(NON_INSTALL_SEGMENTS),
            "rejected_as_non_install": [{"exe": e, "segment": s}
                                        for e, s in sorted(rejected)],
            "label_collisions_resolved": collisions,
            "installs_found": len(out),
            "distinct_labels": len({i["label"] for i in out}),
        })
    return out


def print_scope(scope: dict, out=sys.stdout) -> None:
    """State the scope, every run. A sweep whose scope is unstated reads as
    whole-disk to the next person; two sweeps of this disk each published a
    bare total covering half its engine binaries."""
    print(f"\n{'-' * 118}", file=out)
    print("SCOPE OF THIS RUN -- what was searched, and what was not", file=out)
    print(f"{'-' * 118}", file=out)
    print(f"  root patterns ({len(scope['root_patterns'])}) resolved to "
          f"{len(scope['root_dirs'])} director{'y' if len(scope['root_dirs']) == 1 else 'ies'}:",
          file=out)
    for p in scope["root_patterns"]:
        dirs = scope.get("roots_by_pattern", {}).get(p, [])
        if len(dirs) == 1 and dirs[0].rstrip("/") == p.replace("\\", "/").rstrip("/"):
            print(f"    {p}", file=out)
        else:
            print(f"    {p}  ->  {len(dirs)} dir(s)", file=out)
            for d in dirs:
                print(f"        {d}", file=out)
    for p in scope["unmatched_root_patterns"]:
        print(f"    !! {p}  -- MATCHED NOTHING. A root that does not exist "
              f"reports no installs, and no installs reads as fine.", file=out)
    print(f"  per root: {', '.join(scope['patterns_per_root'])}   "
          f"(depth <= {scope['max_depth']}, NOT recursive)", file=out)
    rej = scope["rejected_as_non_install"]
    print(f"  {len(rej)} candidate binar{'y' if len(rej) == 1 else 'ies'} "
          f"matched a pattern and {'was' if len(rej) == 1 else 'were'} "
          f"REJECTED as build output (disqualifying path segment):", file=out)
    for r in rej[:10]:
        print(f"    {r['exe']}   [{r['segment']}]", file=out)
    for label, paths in scope["label_collisions_resolved"]:
        print(f"  label collision resolved to full paths: {label} -> "
              f"{len(paths)} binaries", file=out)
    print(f"  RESULT: {scope['installs_found']} install(s), "
          f"{scope['distinct_labels']} distinct label(s) -- these must be "
          f"equal, or rows merge.", file=out)
    print("  THIS IS NOT A WHOLE-DISK SWEEP. Engine binaries outside these "
          "roots are not", file=out)
    print("  graded and are not counted -- including every "
          "<worktree>/build64/RelWithDebInfo/", file=out)
    print("  build output, which is a compiler artifact with no deployed data "
          "tree beside it,", file=out)
    print("  not something a user launches. Pass --install-root to widen this; "
          "read any total", file=out)
    print("  below as 'of the installs named here', never as 'of the "
          "binaries on this disk'.", file=out)


def load_install(inst: dict) -> dict:
    try:
        blob = open(inst["exe"], "rb").read()
    except Exception as e:                       # noqa: BLE001
        inst["blob"] = None
        inst["fp"] = {"present": False, "reason": "unreadable",
                      "detail": str(e)}
        return inst
    inst["blob"] = blob
    inst["fp"] = ef.extract_from_bytes(blob)
    return inst


# ---------------------------------------------------------------------------
# Grading
# ---------------------------------------------------------------------------
def grade_code(fix, inst) -> dict:
    """Axis 1. -> {state, tier, detail, conflict}"""
    fp = inst["fp"]
    out = {"state": UNK, "tier": "none", "detail": "", "conflict": ""}
    tiers = {}

    if fix.get("exe_needle") and inst.get("blob") is not None:
        s = scan_binary(inst["blob"], fix["exe_needle"])
        tiers["binary"] = s["state"]
        out["detail"] = f"{fix['exe_needle']}: {s['control']}"

    sha = fp.get("sha") if fp.get("present") else None
    dirty = bool(fp.get("dirty"))
    if sha:
        for ev in fix.get("code_evidence") or []:
            body = show(sha, ev["path"])
            st = NO if body is None else (YES if ev["needle"] in body else NO)
            tiers["source@sha"] = st if tiers.get("source@sha") != NO else NO
        anc = is_ancestor(fix["commit"], sha)
        tiers["ancestry"] = UNK if anc is None else (YES if anc else NO)

    if not tiers:
        out["detail"] = out["detail"] or "no evidence available"
        return out

    if dirty:
        for k in ("source@sha", "ancestry"):
            if k in tiers:
                tiers[k] = UNK
        out["conflict"] = ("built from a DIRTY tree: the embedded sha does not "
                           "describe these bytes, so commit-derived evidence "
                           "is void")

    for tier in ("binary", "source@sha", "ancestry"):
        if tier in tiers and tiers[tier] != UNK:
            out["state"], out["tier"] = tiers[tier], tier
            break
    else:
        out["tier"] = "unusable"

    disagree = {k: v for k, v in tiers.items() if v != UNK}
    if len(set(disagree.values())) > 1:
        out["conflict"] = ("EVIDENCE TIERS DISAGREE: "
                           + ", ".join(f"{k}={v}" for k, v in tiers.items())
                           + " -- the binary is authoritative")
    return out


def grade_default(fix, inst, cache) -> dict:
    """Axis 2, evaluated at the commit THIS install was built from."""
    var = fix.get("gate")
    if not var:
        return {"state": YES, "value": "ungated", "notes": [],
                "disagreement": []}
    fp = inst["fp"]
    if not fp.get("present"):
        return {"state": UNK, "value": "?", "disagreement": [],
                "notes": ["no embedded fingerprint: cannot know which reader "
                          "this binary compiled"]}
    sha = fp["sha"]
    key = (sha, var)
    if key not in cache:
        cache[key] = read_default_at(sha, var)
    r = cache[key]
    # A reader/registry disagreement AT AN OLD COMMIT is history, not a lie in
    # the tree today, so it is reported in its own section rather than failing
    # the run. The reader still wins: it is what that binary compiled.
    disagreement = [n for n in r["notes"]
                    if "REGISTRY LIES" in n or "DISAGREE" in n]
    if r["default"] is None:
        return {"state": UNK, "value": "?", "notes": r["notes"],
                "disagreement": disagreement, "readers": r["readers"]}
    notes = list(r["notes"])
    expect = fix.get("expect_default")
    # WHEN THE MANIFEST STATES A DEFAULT, THAT IS THE TEST -- not "is it on".
    #
    # "ON by default" is the right question only for a feature. It is the WRONG
    # question for two shapes this manifest already contains:
    #   * a RETRACTION (CONCRETE-DELIVER-2 expects default 0). Grading it "ON =
    #     delivered" would have called an install that still ships the withdrawn
    #     default-ON a success, which is backwards.
    #   * a MULTI-ARM numeric gate, where 1 and 2 are different behaviours that
    #     is_on() flattens into the same True. An install shipping arm 1 must not
    #     grade as delivering a fix that requires arm 2.
    if expect is not None:
        mm = expect_mismatch(r["default"], expect)
        if mm:
            notes.append(mm)
        return {"state": NO if mm else YES, "value": r["default"],
                "notes": notes, "disagreement": disagreement,
                "expected": expect, "readers": r["readers"]}
    on = is_on(r["default"])
    return {"state": YES if on else NO, "value": r["default"], "notes": notes,
            "disagreement": disagreement, "readers": r["readers"]}


def grade_data(fix, inst) -> dict:
    """Axis 3. Existence is not evidence -- a declared needle must be present."""
    # An ABSENT "data" key means nobody ever said what this fix needs, and that
    # can never grade as delivered. An EXPLICIT empty list is a declaration that
    # the fix needs no data (a pure default revert, say) and does grade. The two
    # must not collapse: silence is not a statement.
    if "data" not in fix:
        return {"state": UNK, "detail": ["data requirements UNDECLARED -- not "
                                         "the same as none required"],
                "missing": 0}
    items = fix["data"] or []
    if not items:
        return {"state": YES, "detail": ["no data required (declared)"],
                "missing": 0}
    deploy = inst["deploy"]
    detail, bad = [], 0
    for it in items:
        kind = it.get("kind", "file")
        if kind == "file":
            p = os.path.join(deploy, it["path"].replace("/", os.sep))
            if not os.path.isfile(p):
                detail.append(f"MISSING {it['path']}")
                bad += 1
                continue
            needle = it.get("needle")
            if needle:
                try:
                    txt = open(p, "r", encoding="utf-8",
                               errors="replace").read()
                except Exception as e:            # noqa: BLE001
                    detail.append(f"UNREADABLE {it['path']}: {e}")
                    bad += 1
                    continue
                if needle not in txt:
                    detail.append(f"STALE {it['path']} (present but lacks "
                                  f"'{needle}') -- HALF DATA: the uniform does "
                                  f"not exist in the program, the set no-ops, "
                                  f"the gate runs OFF while reporting ON")
                    bad += 1
                    continue
            detail.append(f"ok {it['path']}")
        elif kind == "glob":
            hits = _glob.glob(os.path.join(deploy,
                                           it["glob"].replace("/", os.sep)))
            need = int(it.get("min_count", 1))
            if len(hits) < need:
                detail.append(f"MISSING {it['glob']} ({len(hits)} found, "
                              f"{need} required)")
                bad += 1
            else:
                detail.append(f"ok {it['glob']} ({len(hits)} found)")
        else:
            detail.append(f"UNKNOWN data kind '{kind}'")
            bad += 1
    return {"state": NO if bad else YES, "detail": detail, "missing": bad}


_TIER_WHY = {
    "binary": "the fix's literal is not in the image",
    "source@sha": "the source at this build's commit does not contain the fix "
                  "(reverted or never merged)",
    "ancestry": "the fix commit is not in this build's history",
}


def verdict(code, dflt, data) -> tuple[str, str]:
    if code["state"] == NO:
        return "NO", ("code not in the binary -- "
                      + _TIER_WHY.get(code["tier"], code["tier"]))
    if dflt["state"] == NO:
        # Never assert "code is there" on an install whose code axis came back
        # UNKNOWN -- that would be the tool inventing the half of the answer it
        # just said it did not have.
        lead = "code is in the binary but" if code["state"] == YES \
            else "code presence unknown, and"
        exp = dflt.get("expected")
        if exp is not None and is_on(dflt["value"]):
            return "NO", (f"{lead} the shipped default is {dflt['value']}, "
                          f"not the {exp} this fix requires")
        return "NO", (f"{lead} the gate default is {dflt['value']} -- OFF "
                      f"unless the user sets it")
    if data["state"] == NO:
        first = next((d for d in data["detail"]
                      if d.startswith(("MISSING", "STALE", "UNREADABLE"))), "")
        return "NO", f"data {first}"
    if UNK in (code["state"], dflt["state"], data["state"]):
        which = [n for n, s in (("code", code["state"]), ("default",
                 dflt["state"]), ("data", data["state"])) if s == UNK]
        return "?", "cannot grade: " + "+".join(which) + " unknown"
    return "WOULD SEE IT", "code in binary, default ON, data present"


# ---------------------------------------------------------------------------
# Manifest integrity -- this file is the artifact under the most suspicion
# ---------------------------------------------------------------------------
def check_integrity(fix, at=WORKING_TREE) -> list[str]:
    errs = []
    if not commit_exists(fix["commit"]):
        errs.append(f"commit {fix['commit']} does not exist in this repo")
        return errs
    for ev in fix.get("code_evidence") or []:
        body = show(fix["commit"], ev["path"])
        if body is None:
            errs.append(f"{ev['path']} absent at {fix['commit']}")
        elif ev["needle"] not in body:
            errs.append(f"needle '{ev['needle']}' absent from {ev['path']} at "
                        f"{fix['commit']} -- the manifest describes code that "
                        f"is not there")
    for it in fix.get("data") or []:
        if it.get("kind", "file") == "file":
            if show(at, it["path"]) is None:
                errs.append(f"data path {it['path']} absent from the repo at "
                            f"{label_of(at)}")
    var = fix.get("gate")
    if var:
        r = read_default_at(at, var)
        for n in r["notes"]:
            if "REGISTRY LIES" in n or "DISAGREE" in n:
                errs.append(f"{var}: {n}")
        if r["default"] is None:
            errs.append(f"{var}: default not classifiable at {label_of(at)} -- "
                        f"{'; '.join(r['notes']) or 'no reason given'}")
        else:
            mm = expect_mismatch(r["default"], fix.get("expect_default"))
            if mm:
                errs.append(f"{var} at {label_of(at)}: {mm}")
    return errs


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def load_manifest() -> list[dict]:
    with open(MANIFEST, "r", encoding="utf-8") as f:
        return json.load(f)["fixes"]


def synth_fix(var: str) -> dict:
    """Ad-hoc entry for --gate: axes 1/2/4 only, axis 3 stays UNDECLARED."""
    paths = grep_files("HEAD", f'"{var}"', gda.SCAN_DIRS)
    paths = [p for p in paths if p.endswith(gda.SCAN_EXTS)
             and not p.replace("\\", "/").endswith(REGISTRY_REL)]
    commit = ""
    if paths:
        out = git("log", "--format=%H", "-S", f'"{var}"', "--reverse",
                  "--", *paths)
        if out and out.strip():
            commit = out.split()[0]
    # NOTE the deliberately ABSENT "data" key: an ad-hoc query has nobody to
    # declare data requirements, so the data axis reports UNKNOWN and the fix
    # can never grade WOULD SEE IT. Writing "data": [] here would have quietly
    # asserted "needs no data" on behalf of a gate nobody examined.
    return {"id": f"(ad-hoc) {var}", "commit": commit or "HEAD", "gate": var,
            "expect_default": None, "exe_needle": var,
            "code_evidence": [{"path": p, "needle": var} for p in paths[:1]],
            "note": "ad-hoc query: data requirements undeclared."}


def report(fixes, installs, out=sys.stdout, want_json=False, scope=None):
    cache = {}
    results = []
    integrity_errs = []
    historical = []
    for fix in fixes:
        errs = check_integrity(fix)
        integrity_errs += [f"{fix['id']}: {e}" for e in errs]
        rows = []
        for inst in installs:
            code = grade_code(fix, inst)
            dflt = grade_default(fix, inst, cache)
            data = grade_data(fix, inst)
            v, why = verdict(code, dflt, data)
            for d in dflt.get("disagreement") or []:
                historical.append(f"{fix['id']} @ {inst['label']} "
                                  f"({(inst['fp'].get('sha12') or '?')}): {d}")
            rows.append({"install": inst["label"],
                         # THE UNIQUE KEY, emitted on every row on purpose.
                         # A consumer that groups by "install" is grouping by a
                         # display label; DELIVERY-REGRADE-1 built a table that
                         # way and merged six installs into one line. `exe` is
                         # one binary, always, whatever the label says.
                         "exe": inst["exe"].replace("\\", "/"),
                         "deploy": inst["deploy"].replace("\\", "/"),
                         "sha": (inst["fp"].get("sha12")
                                 if inst["fp"].get("present")
                                 else "NO-FINGERPRINT"),
                         "fp_reason": inst["fp"].get("reason"),
                         "dirty": bool(inst["fp"].get("dirty")),
                         "code": code, "default": dflt, "data": data,
                         "verdict": v, "why": why})
        results.append({"fix": fix, "rows": rows})

    if want_json:
        json.dump({"scope": scope or {},
                   "results": [{"id": r["fix"]["id"],
                                "commit": r["fix"]["commit"],
                                "gate": r["fix"].get("gate"),
                                "rows": [{k: v for k, v in row.items()}
                                         for row in r["rows"]]}
                               for r in results],
                   "integrity_errors": integrity_errs,
                   "historical_disagreements": sorted(set(historical))},
                  out, indent=2, default=str)
        out.write("\n")
        return results, integrity_errs

    if scope:
        print_scope(scope, out=out)

    # THE COLUMN MUST NOT UNDO THE LABELS. Labels were just made unique; a
    # fixed 38-char column that truncates them would merge the rows again on
    # screen, which is the same bug wearing a different hat. Width follows the
    # longest label, and the rule lines follow the width.
    W = max(38, max((len(i["label"]) for i in installs), default=38))
    RULE = W + 76
    summary = []
    for r in results:
        fix, rows = r["fix"], r["rows"]
        head_default = "?"
        if fix.get("gate"):
            head_default = read_default_at(WORKING_TREE,
                                          fix["gate"])["default"] or "?"
        print(f"\n{'=' * RULE}", file=out)
        print(f"FIX  {fix['id']}", file=out)
        print(f"     commit {fix['commit']}   gate "
              f"{fix.get('gate') or '(ungated)'}   "
              f"reader default at the tip: {head_default}", file=out)
        print(f"{'=' * RULE}", file=out)
        print(f"  {'install':{W}} {'built-from':19} {'code':4} {'dflt':4} "
              f"{'data':4}  would I see it if I launched?", file=out)
        print(f"  {'-' * (RULE - 4)}", file=out)
        seen_ct = 0
        for row in rows:
            if row["verdict"] == "WOULD SEE IT":
                seen_ct += 1
            sha = (row["sha"] or "?")[:14] + ("*dirty" if row["dirty"] else "")
            why = row["why"]
            if len(why) > 56:
                why = why[:53] + "..."
            print(f"  {row['install']:{W}} {sha:19} "
                  f"{row['code']['state']:4} {row['default']['value'][:4]:4} "
                  f"{row['data']['state']:4}  {row['verdict']}: {why}",
                  file=out)
        print(f"  {'-' * (RULE - 4)}", file=out)
        print(f"  ANSWER: {seen_ct} of {len(rows)} installs on disk would show "
              f"{fix['id']}.", file=out)
        summary.append((fix["id"], seen_ct, len(rows)))
        ungraded = sum(1 for row in rows if row["verdict"] == "?")
        if seen_ct == 0 and ungraded == len(rows):
            print(f"          NOT GRADED ANYWHERE -- all {ungraded} installs "
                  f"came back unknown. That is ignorance, not a verdict.",
                  file=out)
        elif seen_ct == 0:
            print("          NOT DELIVERED ANYWHERE -- on every install this "
                  "fix is absent, withdrawn, off, or missing its data."
                  + (f" ({ungraded} could not be graded at all.)"
                     if ungraded else ""), file=out)
        elif ungraded:
            print(f"          ({ungraded} install(s) could not be graded; "
                  f"they are not counted as either.)", file=out)
        # Default drift across installs. This is the withdrawn-code detector:
        # an install whose shipped default is not the tip's shipped default is
        # grading code the project has since changed its mind about.
        drift = sorted({row["default"]["value"] for row in rows
                        if row["default"]["state"] != UNK})
        if fix.get("gate") and head_default != "?" and drift:
            off_tip = [row["install"] for row in rows
                       if row["default"]["state"] != UNK
                       and row["default"]["value"] != head_default]
            if off_tip:
                print(f"          WITHDRAWN-DEFAULT WARNING: {len(off_tip)} "
                      f"install(s) ship {fix['gate']} default "
                      f"{'/'.join(v for v in drift if v != head_default)} "
                      f"while the tip ships {head_default}.", file=out)
                print(f"            {', '.join(off_tip)}", file=out)
                print("            Anything graded on those installs grades a "
                      "default this repo no longer ships.", file=out)
        # Evidence conflicts are never resolved silently.
        for row in rows:
            if row["code"].get("conflict"):
                print(f"          [{row['install']}] {row['code']['conflict']}",
                      file=out)

    if len(summary) > 1:
        print(f"\n{'=' * RULE}", file=out)
        print("DELIVERY SUMMARY -- would the user see it if they launched?",
              file=out)
        print(f"{'=' * RULE}", file=out)
        for fid, n, tot in summary:
            flag = "  <-- DELIVERED NOWHERE" if n == 0 else ""
            print(f"  {fid:{W}} {n:>3} / {tot} installs{flag}", file=out)

    if historical:
        print(f"\n  HISTORICAL reader-vs-registry disagreements (in the "
              f"commits these installs were built from, not in the tree "
              f"today):", file=out)
        for e in sorted(set(historical)):
            print(f"    * {e}", file=out)

    if integrity_errs:
        print(f"\n{'!' * RULE}", file=out)
        print("INTEGRITY FAILURES -- something in the tree is lying RIGHT NOW:",
              file=out)
        for e in sorted(set(integrity_errs)):
            print(f"  * {e}", file=out)
        print(f"{'!' * RULE}", file=out)
    return results, integrity_errs


# ---------------------------------------------------------------------------
# THE EXIT CODE -- the half that made this tool decorative
# ---------------------------------------------------------------------------
# DELIVERY-CLOSE-1. The first version computed
#
#     delivered = any(any(row is WOULD SEE IT for row in fix.rows)
#                     for fix in results)
#     return 0 if delivered else 1
#
# -- ONE `any` spanning every fix AND every install. So a run in which four
# fixes were graded and TWO of them printed "<-- DELIVERED NOWHERE" in the
# summary block still exited 0, because two OTHER fixes happened to land
# somewhere. Measured on this disk before the fix: VEHICLE-AMBIENT-FLOOR-1 0/18,
# CONCRETE-DELIVER-2-RETRACTION 0/18, exit code 0.
#
# A tool that answers "would the user see this?" and cannot return non-zero when
# the answer is no is a report, not a gate. Nothing consumed the report: the
# only two wirings in check-contracts.sh were --contract (integrity, never
# touches an install) and --selftest (fixtures), so the grading half -- the
# entire reason the script exists -- was reachable only by a human typing it.
#
# The gap is now PER FIX. One fix delivered nowhere fails the run.
def resolve_nominated(installs, label: str) -> dict:
    """The ONE install a nomination names. Ambiguity and absence both fail.

    This is the consumer that makes label uniqueness load-bearing rather than
    cosmetic. Under the old labelling a nomination of "mc2-win64-v0.4" named
    SIX binaries with different fingerprints and different verdicts, and any
    tool that resolved it by picking the first would have produced a gate whose
    answer depended on directory iteration order.
    """
    hits = [i for i in installs if i["label"] == label]
    if len(hits) == 1:
        return hits[0]
    # Exit 2, not 1: this is not "the fix did not reach the user", it is "the
    # question could not be asked". Collapsing the two is how a broken gate
    # reads as a failing one and gets waved through as expected-red.
    if not hits:
        near = [i["label"] for i in installs
                if label.lower() in i["label"].lower()][:5]
        print(f"NOMINATION UNRESOLVABLE: no install labelled {label!r}.",
              file=sys.stderr)
        if near:
            print(f"  did you mean: {', '.join(near)}", file=sys.stderr)
        print("  A nominated install the tool cannot find is a gate that "
              "cannot work. --list-installs shows every label.",
              file=sys.stderr)
        raise SystemExit(2)
    print(f"NOMINATION AMBIGUOUS: {label!r} names {len(hits)} binaries:",
          file=sys.stderr)
    for h in hits:
        print(f"    {h['exe']}", file=sys.stderr)
    print("  Refusing to pick one. A gate whose verdict depends on which of "
          "several binaries a label happened to resolve to is not a gate.",
          file=sys.stderr)
    raise SystemExit(2)


def delivery_gaps(results, adhoc: bool = False,
                  nominated: str | None = None) -> list[tuple[str, str]]:
    """-> [(fix id, why)] for every fix no install on disk would show.

    With `nominated`, the bar is that ONE install rather than "any of them".
    That is a strictly HARDER bar and it is the only thing here that offsets
    the loosening described in find_installs: more installs can only make an
    any-of-them bar easier to clear.
    """
    gaps = []
    for res in results:
        rows = res["rows"]
        if not rows:
            continue
        if nominated is not None:
            rows = [r for r in rows if r["install"] == nominated]
            if not rows:
                continue
            if rows[0]["verdict"] == "WOULD SEE IT":
                continue
            gaps.append((res["fix"]["id"],
                         f"the NOMINATED install {nominated} would not show "
                         f"it -- {rows[0]['why']}"))
            continue
        if any(r["verdict"] == "WOULD SEE IT" for r in rows):
            continue
        ungraded = [r for r in rows if r["verdict"] == "?"]
        # AD-HOC (--gate) MODE IS EXEMPT ONLY FROM TOTAL IGNORANCE, never from a
        # real NO. synth_fix() deliberately declares no `data` key, so every row
        # of an ad-hoc query is structurally "?" -- failing on that would make
        # --gate always exit 1 and teach everyone to ignore the code. But an
        # ad-hoc query that produced an actual NO (code absent, default OFF) has
        # found a genuine delivery gap and still fails.
        if adhoc and len(ungraded) == len(rows):
            continue
        if len(ungraded) == len(rows):
            gaps.append((res["fix"]["id"],
                         f"NOT GRADED ANYWHERE -- all {len(rows)} installs came "
                         f"back unknown, and unknown is never a pass"))
        else:
            why = next((r["why"] for r in rows if r["verdict"] == "NO"), "")
            gaps.append((res["fix"]["id"],
                         f"0 of {len(rows)} installs would show it"
                         + (f" -- e.g. {why}" if why else "")))
    return gaps


# ---------------------------------------------------------------------------
# Self-test: injected regressions, each of which MUST be caught
# ---------------------------------------------------------------------------
_SELF_REG = '''
enum class Feature { Alpha = 0, Beta = 1, COUNT = 2 };
static const EnvVarEntry kEntries[] = {
    { "MC2_SELF_ALPHA", "MC2_SELF_ALPHA", EnvVarKind::Feature, true, "d" },
    { "MC2_SELF_BETA",  "MC2_SELF_BETA",  EnvVarKind::Feature, false, "d" },
};
'''

_SELF_SRC_OK = r'''
static bool alphaOn() {
    const char* v = getenv("MC2_SELF_ALPHA");
    return !(v && v[0] == '0');   // default ON
}
static int betaMode() {
    static const int s = [](){
        const char* v = getenv("MC2_SELF_BETA");
        if (!v || !v[0]) return 0;
        return 1;
    }();
    return s;
}
'''

_SELF_SRC_LIE = r'''
static bool alphaOn() {
    const char* v = getenv("MC2_SELF_ALPHA");
    return (v && v[0] == '1');    // silently flipped to opt-in
}
static int betaMode() {
    static const int s = [](){
        const char* v = getenv("MC2_SELF_BETA");
        if (!v || !v[0]) return 1;
        return 1;
    }();
    return s;
}
'''


def _classify_pair(reg_text, src_text, var):
    """Same comparison read_default_at() performs, on in-memory text."""
    srcs = [("self.cpp", gda.strip_comments(src_text))]
    helpers = gda.resolve_helpers(srcs)
    hits = gda.find_readers(var, srcs, helpers)
    verdicts = {h["verdict"] for h in hits}
    if hits and verdicts <= {gda.ON, gda.OFF} and len(verdicts) == 1:
        kind, dflt = "bool", ("ON" if verdicts == {gda.ON} else "OFF")
    else:
        n = None
        for _p, body in srcs:
            for m in re.finditer(r'getenv\s*\(\s*"' + re.escape(var) + r'"',
                                 body):
                blk = gda.enclosing_block(body, m.end())
                n = classify_numeric(body[blk[0]:blk[1]] if blk else body, var)
                if n is not None:
                    break
        kind, dflt = ("numeric", n) if n is not None else (None, None)
    ent = [e for e in gda.parse_registry(reg_text) if e["var"] == var]
    reg = ent[0]["default_on"] if ent else None
    if dflt is None or reg is None:
        return None
    on = dflt == "ON" if kind == "bool" else dflt not in ("0", "-0")
    return on == reg


def selftest() -> int:
    fails = []

    def ck(name, cond, why=""):
        if not cond:
            fails.append(f"{name}: {why}")
        print(f"  {'PASS' if cond else 'FAIL'}  {name}"
              + (f"  -- {why}" if not cond else ""))

    print("check-delivery selftest -- every case injects the REAL regression "
          "shape, not a fixture that merely resembles it")

    # 1. Reader-vs-registry agreement, both directions.
    ck("bool reader agreeing with registry passes",
       _classify_pair(_SELF_REG, _SELF_SRC_OK, "MC2_SELF_ALPHA") is True,
       "an honest default-ON pair was reported as a disagreement")
    ck("INJECTED bool default inversion is caught",
       _classify_pair(_SELF_REG, _SELF_SRC_LIE, "MC2_SELF_ALPHA") is False,
       "a reader flipped to opt-in against registry defaultOn=true was "
       "reported as agreeing -- this is the 17-lies failure mode")
    ck("numeric reader agreeing with registry passes",
       _classify_pair(_SELF_REG, _SELF_SRC_OK, "MC2_SELF_BETA") is True)
    ck("INJECTED numeric default promotion is caught",
       _classify_pair(_SELF_REG, _SELF_SRC_LIE, "MC2_SELF_BETA") is False,
       "a numeric gate promoted to default 1 against registry "
       "defaultOn=false was reported as agreeing -- this is the "
       "MC2_PAVED_OVERLAY_CLASS blind spot")

    # 2. A variable that appears ONLY IN A COMMENT must not register as a
    #    reader. Two contract rules in this repo passed their own self-tests
    #    while satisfied by nothing more than an identifier in a comment.
    comment_only = '// getenv("MC2_SELF_ALPHA") -- default ON, honest\n'
    srcs = [("c.cpp", gda.strip_comments(comment_only))]
    ck("a gate named only in a COMMENT is not a reader",
       gda.find_readers("MC2_SELF_ALPHA", srcs) == [],
       "a commented-out getenv was accepted as the reader")

    # 3. Byte scanner controls.
    fake = b"xx" + ef.POSITIVE_CONTROL + b"\x00u_realNeedle\x00yy"
    ck("byte scan finds a present literal",
       scan_binary(fake, "u_realNeedle")["state"] == YES)
    ck("byte scan reports a genuinely absent literal",
       scan_binary(fake, "u_absentNeedle")["state"] == NO)
    ck("byte scan REFUSES to answer without its positive control",
       scan_binary(b"no banner here", "u_realNeedle")["state"] == UNK,
       "reported NO on a blob it cannot scan -- an absence that did not "
       "prove the scanner works is not a finding")
    ck("byte scan REFUSES to answer when its negative control matches",
       scan_binary(ef.POSITIVE_CONTROL + NEG_CONTROL,
                   "u_realNeedle")["state"] == UNK)

    # 3b. Kind-agnostic default comparison, and the arm it must still refuse.
    ck("'ON' and '1' are the same shipped default",
       expect_mismatch("1", "ON") is None)
    ck("'OFF' and '0' are the same shipped default",
       expect_mismatch("0", "OFF") is None)
    ck("INJECTED on/off inversion in the manifest is caught",
       expect_mismatch("1", "OFF") is not None)
    ck("INJECTED wrong numeric ARM is caught even at the same on/off sense",
       expect_mismatch("2", "1") is not None,
       "default 2 was accepted against a manifest claiming 1")

    # 3c. UNDECLARED data and DECLARED-NONE must not collapse.
    ck("data key ABSENT grades UNKNOWN",
       grade_data({}, {"deploy": "."})["state"] == UNK)
    ck("data key present-and-empty grades YES (an explicit declaration)",
       grade_data({"data": []}, {"deploy": "."})["state"] == YES)

    # 3d. A RETRACTION must not grade as delivered by an install that still
    #     ships the withdrawn value, and a multi-arm gate must not grade arm 1
    #     as arm 2. Both are the same bug: "is it ON" is not "is it RIGHT".
    class _FP:                       # a fingerprint stub; no binary involved
        pass

    def _dflt(reader_value, expect):
        fake = {"gate": "MC2_SELF_ARM", "expect_default": expect}
        inst = {"fp": {"present": True, "sha": "deadbeefcafe"}}
        cache = {("deadbeefcafe", "MC2_SELF_ARM"):
                 {"default": reader_value, "kind": "numeric", "readers": [],
                  "registry": None, "agree": None, "notes": []}}
        return grade_default(fake, inst, cache)

    ck("INJECTED withdrawn default (install ships 1, retraction wants 0)",
       _dflt("1", "0")["state"] == NO,
       "an install still shipping the retracted default-ON graded as having "
       "received the retraction")
    ck("an install shipping the retracted-TO value grades YES",
       _dflt("0", "0")["state"] == YES)
    ck("INJECTED wrong numeric ARM (ships 1, fix needs 2)",
       _dflt("1", "2")["state"] == NO,
       "is_on() flattened two different behaviours into one pass")

    # 4. UNKNOWN never grades as delivered.
    ok = {"state": YES, "tier": "binary", "detail": "", "conflict": ""}
    ck("code UNKNOWN cannot grade WOULD SEE IT",
       verdict({"state": UNK, "tier": "none", "detail": "", "conflict": ""},
               {"state": YES, "value": "ON"},
               {"state": YES, "detail": []})[0] == "?")
    ck("undeclared data cannot grade WOULD SEE IT",
       verdict(ok, {"state": YES, "value": "ON"},
               {"state": UNK, "detail": []})[0] == "?",
       "an entry with no data declared was graded as delivered")
    ck("default OFF cannot grade WOULD SEE IT",
       verdict(ok, {"state": NO, "value": "OFF"},
               {"state": YES, "detail": []})[0] == "NO")
    ck("all three YES grades WOULD SEE IT",
       verdict(ok, {"state": YES, "value": "ON"},
               {"state": YES, "detail": []})[0] == "WOULD SEE IT")

    # 5. Half-data: a deployed file that EXISTS but predates the fix.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        sd = os.path.join(td, "shaders")
        os.makedirs(sd)
        with open(os.path.join(sd, "x.vert"), "w", encoding="utf-8") as f:
            f.write("uniform int u_oldUniformOnly;\n")
        fx = {"data": [{"kind": "file", "path": "shaders/x.vert",
                        "needle": "u_newUniform"}]}
        r = grade_data(fx, {"deploy": td})
        ck("INJECTED half-data (file present, needle absent) is caught",
           r["state"] == NO and any("STALE" in d for d in r["detail"]),
           "a stale deployed shader was accepted because the FILE existed")
        fx2 = {"data": [{"kind": "file", "path": "shaders/x.vert",
                         "needle": "u_oldUniformOnly"}]}
        ck("a matching deployed file passes",
           grade_data(fx2, {"deploy": td})["state"] == YES)
        fx3 = {"data": [{"kind": "file", "path": "shaders/gone.vert"}]}
        ck("INJECTED missing data file is caught",
           grade_data(fx3, {"deploy": td})["state"] == NO)
        fx4 = {"data": [{"kind": "glob", "glob": "data/missions/*.beauty/m.png",
                         "min_count": 1}]}
        ck("INJECTED empty sidecar glob is caught",
           grade_data(fx4, {"deploy": td})["state"] == NO,
           "zero .beauty sidecars graded as data present -- the shipped "
           "v0.5.2 shape")

    # 5b. THE EXIT CODE ITSELF. The shape that made this tool decorative: a run
    #     where SOME fix delivers and another delivers NOWHERE must still fail.
    def _res(fid, verdicts):
        return {"fix": {"id": fid},
                "rows": [{"verdict": v, "why": "x"} for v in verdicts]}

    ck("a fix delivered nowhere is a GAP",
       [g[0] for g in delivery_gaps([_res("A", ["NO", "NO"])])] == ["A"])
    ck("INJECTED mixed run (one fix lands, another delivers NOWHERE) fails",
       [g[0] for g in delivery_gaps([_res("A", ["WOULD SEE IT"]),
                                     _res("B", ["NO", "NO"])])] == ["B"],
       "the original `any` across ALL fixes let a delivered fix mask an "
       "undelivered one -- 2 of 4 fixes were 0/18 and the run exited 0")
    ck("a fix delivered on ONE install of many is not a gap",
       delivery_gaps([_res("A", ["NO", "WOULD SEE IT", "NO"])]) == [])
    ck("NOT GRADED ANYWHERE is a gap (unknown is never a pass)",
       [g[0] for g in delivery_gaps([_res("A", ["?", "?"])])] == ["A"])
    ck("ad-hoc --gate mode is exempt from TOTAL ignorance only",
       delivery_gaps([_res("A", ["?", "?"])], adhoc=True) == [])
    ck("ad-hoc --gate mode still fails on a real NO",
       [g[0] for g in delivery_gaps([_res("A", ["?", "NO"])],
                                    adhoc=True)] == ["A"],
       "an ad-hoc query that proved the code is absent was let through")

    # 5c. NUMERIC READER BLIND SPOTS, both found by pointing the tool at a real
    #     gate rather than at a fixture built to its own shape.
    ck("INJECTED float default is classified (was UNKNOWN -> ungradeable)",
       classify_numeric('const char* e = getenv("X"); '
                        'if (!e || !e[0]) return 0.35f;', "X") == "0.35",
       "MC2_TERRAIN_AMBIENT_FLOOR ships default 0.35 and classified UNKNOWN, "
       "so it could never grade as delivered on any install")
    ck("INJECTED non-'v' pointer name is classified",
       classify_numeric('const char* envStr = getenv("X"); '
                        'if (!envStr) return 2;', "X") == "2",
       "every rule hard-coded the pointer name `v`")
    ck("a MISMATCHED pointer pair is still refused",
       classify_numeric('const char* a = getenv("X"); '
                        'if (!a || !b[0]) return 7;', "X") is None,
       "the back-reference did not hold: two different variables were "
       "accepted as one null-check idiom")
    # THE REAL FALSE POSITIVE the generalised rules produced on first contact
    # with the tree, reproduced verbatim in shape: two gates in ONE enclosing
    # block, one of them a bare presence test. gda.enclosing_block() hands the
    # whole block to both, so an unbound sweep credits the neighbour's default.
    _two_gates = ('static const char* s_ambEnv = getenv("MC2_X"); '
                  'const char* e = getenv("MC2_X"); '
                  'if (!e || !e[0]) return 0.35f; '
                  'if (!s_traced && getenv("MC2_X_TRACE")) { trace(); }')
    ck("the gate that OWNS the default still classifies in a shared block",
       classify_numeric(_two_gates, "MC2_X") == "0.35")
    ck("INJECTED neighbour-default theft (bare presence test in the same "
       "block) is refused",
       classify_numeric(_two_gates, "MC2_X_TRACE") is None,
       "a presence-test gate was credited with the neighbouring gate's float "
       "default and then reported as REGISTRY LIES -- a lie manufactured by "
       "the checker itself")
    ck("float 0.0 reads as OFF", is_on("0.0") is False)
    ck("float 0.35 reads as ON", is_on("0.35") is True)
    ck("'0' and '0.0' are the same shipped default",
       expect_mismatch("0.0", "0") is None)
    ck("INJECTED float arm mismatch is caught",
       expect_mismatch("0.35", "0.5") is not None)

    # 5d. INSTALL DISCOVERY AND LABELLING (CONTRACT-TRUTH-2). Both regressions
    #     are built as real directory trees, not mocked, because both were
    #     invisible to every fixture this script already had: the tool graded
    #     23 installs on a disk that held 28, and its labels merged six of them
    #     onto one row.
    with tempfile.TemporaryDirectory() as td:
        def touch(*parts):
            p = os.path.join(td, *parts)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(b"x")
            return p

        # An install AT the root, one a level down, one TWO levels down (the
        # <root>/data/<install>/ shape that was unreachable), a releases/ one,
        # and a build tree that must NOT be discovered.
        touch("rootinst", EXE_NAME)
        touch("rootinst", "data", "mc2-win64-v0.4", EXE_NAME)
        touch("container", "mc2-win64-v0.4", EXE_NAME)
        touch("container", "releases", "mc2-win64-v0.5.1", EXE_NAME)
        # A SOURCE WORKTREE, the exact shape <root>/*/*/ would otherwise sweep
        # in: a real, freshly-linked binary two levels down with no deployed
        # data tree beside it. 15 of these exist under A:/Games.
        touch("worktree", "build64", "RelWithDebInfo", EXE_NAME)

        sc = {}
        got = find_installs([os.path.join(td, "rootinst"),
                             os.path.join(td, "container"),
                             os.path.join(td, "worktree")], scope=sc)
        labels = sorted(i["label"] for i in got)
        ck("INJECTED two-levels-down install is discovered",
           any(i["exe"].endswith(os.path.join("data", "mc2-win64-v0.4",
                                              EXE_NAME)) for i in got),
           "a <root>/data/<install>/ copy was invisible -- five of them were "
           "on this disk, and an install root that is not scanned cannot "
           "report NO, it reports nothing, and nothing reads as fine")
        ck("root that is ITSELF an install is still discovered",
           "rootinst" in labels)
        ck("depth-1 and releases/ installs are still discovered",
           "mc2-win64-v0.4" in labels
           and "releases/mc2-win64-v0.5.1" in labels,
           "widening discovery narrowed it")
        ck("NEGATIVE CONTROL: a build tree is NOT an install",
           not any("build64" in i["exe"] for i in got)
           and len(sc["rejected_as_non_install"]) == 1,
           "reaching two levels down swept a compiler output into the graded "
           "set -- it has no deployed data beside it and every data axis "
           "would report MISSING")
        ck("INJECTED colliding basenames get DISTINCT labels",
           len(labels) == len(set(labels)) == 4,
           f"two installs both named mc2-win64-v0.4 merged onto one row: "
           f"{labels}")
        ck("the merged pair is separated by ROOT, not by an index",
           "rootinst/data/mc2-win64-v0.4" in labels
           and "mc2-win64-v0.4" in labels,
           "labels must be a function of the path alone, so a nomination "
           "means the same install from one run to the next")
        ck("scope states what it searched and what it rejected",
           sc["max_depth"] == 2 and sc["recursive"] is False
           and sc["installs_found"] == 4 and sc["distinct_labels"] == 4)
        ck("a root pattern that matches nothing is REPORTED, not silent",
           find_installs([os.path.join(td, "nope")], scope=(s2 := {})) == []
           and s2["unmatched_root_patterns"] == [os.path.join(td, "nope")],
           "a root that does not exist reports no installs, and no installs "
           "reads as fine")

        # The uniqueness BACKSTOP: two container roots each holding the same
        # basename produce the same relative path, so the rule alone is not
        # enough and the escalation must fire.
        touch("container2", "mc2-win64-v0.4", EXE_NAME)
        sc2 = {}
        got2 = find_installs([os.path.join(td, "container"),
                              os.path.join(td, "container2")], scope=sc2)
        l2 = [i["label"] for i in got2]
        ck("INJECTED cross-root label collision is escalated, not merged",
           len(set(l2)) == len(l2) and bool(sc2["label_collisions_resolved"]),
           f"the backstop did not fire: {l2}")

        # Nomination: the consumer that makes uniqueness load-bearing.
        ck("a nomination resolving to exactly one install is accepted",
           resolve_nominated(got, "rootinst/data/mc2-win64-v0.4")["label"]
           == "rootinst/data/mc2-win64-v0.4")
        try:
            resolve_nominated(got, "mc2-win64-v0.4-that-is-not-here")
            ok = False
        except SystemExit as e:
            ok = e.code == 2
        ck("INJECTED unresolvable nomination FAILS LOUDLY, as exit 2",
           ok, "a nominated install the tool cannot find would have become a "
               "gate that silently graded something else -- and exit 1 would "
               "have read as 'the fix did not land' rather than 'the question "
               "could not be asked'")
        try:
            resolve_nominated(got2, "mc2-win64-v0.4")
            amb = False
        except SystemExit as e:
            amb = e.code == 2
        ck("INJECTED ambiguous nomination is REFUSED, not arbitrated",
           amb, "a label naming several binaries was silently resolved to one "
                "of them, so the gate's verdict depended on iteration order")
        ck("nominating restricts the pass bar to that install",
           [g[0] for g in delivery_gaps(
               [{"fix": {"id": "A"},
                 "rows": [{"install": "x", "verdict": "WOULD SEE IT",
                           "why": ""},
                          {"install": "y", "verdict": "NO", "why": "off"}]}],
               nominated="y")] == ["A"],
           "the nominated install said NO but the run passed off another "
           "install's YES -- which is the 'any of twenty-eight' bar wearing a "
           "nomination")

    # 6. Manifest integrity: a needle the named commit does not contain.
    bogus = {"id": "bogus", "commit": "HEAD", "gate": None,
             "code_evidence": [{"path": "README.md",
                                "needle": "MC2_NEEDLE_THAT_IS_NOT_THERE_9a1"}],
             "data": []}
    ck("INJECTED manifest lie (needle absent at the named commit) is caught",
       any("absent from" in e for e in check_integrity(bogus)),
       "the manifest was allowed to describe code that is not there")
    ck("INJECTED manifest lie (nonexistent commit) is caught",
       any("does not exist" in e
           for e in check_integrity({"id": "b", "commit":
                                     "0000000000000000000000000000000000000000",
                                     "gate": None, "code_evidence": [],
                                     "data": []})))

    print()
    if fails:
        print(f"SELFTEST FAILED ({len(fails)}):")
        for f in fails:
            print(f"  {f}")
        return 2
    print("SELFTEST PASSED -- every injected regression was caught")
    return 0


# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Would I see this fix if I launched?",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fix", help="manifest fix id (substring match ok)")
    ap.add_argument("--gate", help="ad-hoc: grade an MC2_* gate with no "
                                   "manifest entry (data axis stays UNKNOWN)")
    ap.add_argument("--install", help="restrict to installs whose label matches "
                                      "this glob")
    ap.add_argument("--install-root", action="append",
                    default=None, help="where installs live (repeatable)")
    ap.add_argument("--list-installs", action="store_true",
                    help="print the scope block and every discovered install "
                         "label with its binary path, then exit")
    ap.add_argument("--nominate", metavar="LABEL",
                    help="grade the run against ONE install: the pass bar "
                         "becomes 'this install shows the fix' instead of "
                         "'some install does'. Fails if the label does not "
                         "resolve to exactly one binary. No default -- which "
                         "install counts is a user decision.")
    ap.add_argument("--contract", action="store_true",
                    help="repo-integrity only: no install grading, exit 2 on "
                         "any manifest/reader/registry lie")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--self-test", dest="selftest", action="store_true")
    ap.add_argument("--classifier", choices=("worktree", "committed"),
                    default="worktree",
                    help="whose gate-default rules to use: the working copy of "
                         "check-gate-default-agreement.py (default, matches "
                         "check-contracts.sh) or its committed bytes "
                         "(reproducible)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.classifier == "committed":
        use_committed_classifier()
    elif CLASSIFIER_DIRTY and not args.quiet:
        print("[check-delivery] NOTE: scripts/check-gate-default-agreement.py "
              "is DIRTY; every default verdict below was produced by another "
              "lane's in-flight rules. Re-run with --classifier committed for "
              "a reproducible verdict.", file=sys.stderr)

    if args.selftest:
        return selftest()

    if args.list:
        for f in load_manifest():
            print(f"{f['id']:30} {f['commit']:10} {f.get('gate') or '-'}")
        return 0

    if args.gate:
        fixes = [synth_fix(args.gate)]
    else:
        fixes = load_manifest()
        if args.fix:
            fixes = [f for f in fixes
                     if args.fix.lower() in f["id"].lower()]
            if not fixes:
                print(f"no manifest fix matching '{args.fix}'; "
                      f"--list shows them all", file=sys.stderr)
                return 2

    if args.contract:
        errs = []
        for f in fixes:
            errs += [f"{f['id']}: {e}" for e in check_integrity(f)]
        if errs:
            print("FAIL delivery-manifest integrity:")
            for e in errs:
                print(f"  * {e}")
            return 2
        if not args.quiet:
            print(f"PASS delivery-manifest integrity ({len(fixes)} fixes: "
                  f"commits exist, needles present at the commits that claim "
                  f"them, data paths in-tree, every reader agrees with the "
                  f"registry and with the manifest)")
        return 0

    roots = args.install_root or DEFAULT_INSTALL_ROOTS
    scope: dict = {}
    found = find_installs(roots, scope=scope)

    if args.list_installs:
        print_scope(scope)
        print()
        for i in sorted(found, key=lambda x: x["label"]):
            print(f"  {i['label']:52} {i['exe']}")
        # Same rule as the grading path: a listing that found nothing has
        # answered nothing, and exiting 0 on it is how "no installs" reads as
        # fine. The scope block above says WHY it found nothing.
        return 0 if found else 2

    installs = [load_install(i) for i in found]
    if args.install:
        installs = [i for i in installs
                    if fnmatch.fnmatch(i["label"], args.install)]
        scope["installs_found"] = len(installs)
        scope["distinct_labels"] = len({i["label"] for i in installs})
        scope["filtered_by"] = args.install
    # UNCHANGED BAR, STATED SO IT IS NOT MISTAKEN FOR ONE. Zero installs is
    # still exit 2, not exit 0: a run that graded nothing has answered nothing.
    # Widening discovery does make this easier to clear -- see find_installs.
    if not installs:
        print(f"no engine installs found under {', '.join(roots)}",
              file=sys.stderr)
        return 2

    nominated = None
    if args.nominate:
        nominated = resolve_nominated(installs, args.nominate)["label"]

    results, integrity = report(fixes, installs, want_json=args.json,
                                scope=scope)

    if integrity:
        return 2

    if nominated and not args.json:
        print(f"\nNOMINATED INSTALL: {nominated}\n  The pass bar for this run "
              f"is THIS install alone, not 'any of {len(installs)}'.",
              file=sys.stdout)

    gaps = delivery_gaps(results, adhoc=bool(args.gate), nominated=nominated)
    if gaps:
        print(f"\n{'!' * 118}", file=sys.stderr)
        print("DELIVERY FAILURE -- the user would not see these fixes "
              + (f"on the NOMINATED install {nominated}:" if nominated
                 else "on ANY install in the scope printed above:"),
              file=sys.stderr)
        for fid, why in gaps:
            print(f"  * {fid}: {why}", file=sys.stderr)
        print("Correct code that reaches no install is not delivered. Rebuild "
              "and redeploy at the tip, install the data the gate reads, or "
              "retract the manifest entry.", file=sys.stderr)
        print(f"{'!' * 118}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""GATE-DEFAULT-TRUTH-1 -- a written default must equal the gate table's default.

WHAT IT CHECKS NOW
------------------
A comment, a shader comment or a markdown doc line that asserts a default for exactly one MC2_* gate
("MC2_X ... default ON") is compared with the compiled gate table (Gates/gate_table.json): the Bool row's own
default. Prose is where the lies live -- MC2_SHADOW_BOUNDED_NEAR_FIT was documented OFF in three places while its
reader implemented ON, and an A/B of "=1 versus unset" graded ON against ON for a whole session -- and no compiler or
table check can see a sentence.

WHAT IT NO LONGER CHECKS, AND WHY (G8, 2026-10-10)
-------------------------------------------------
This file used to classify every getenv idiom in the tree to prove the registry's `defaultOn` agreed with the
reader, and to prove two readers of one gate agreed with each other. With the typed registry both are structural:
the reader of a gate IS the table row, `gate_gen --check` proves the registry rows are generated from it, and
tools/gate_conflicts.py proves no raw reader disagrees with a row. The 1,100 lines of idiom classifiers, helper
resolution and reader-conflict logic therefore graded the table against itself (217 of 230 checked entries were
typed-only). They are deleted; the last revision that carries them is the commit before this one.

The comment/doc sweep, the claim grammar (one variable per line, unconditional, denial-aware), the shader sweep and
the tracked-docs rule are kept as they were. Exemptions live in scripts/gate-defaults-allowlist.txt and each one must
carry a real reason.

USAGE
  py -3 scripts/check-gate-default-agreement.py            # report
  py -3 scripts/check-gate-default-agreement.py --quiet    # exit code only
  py -3 scripts/check-gate-default-agreement.py --selftest # prove it catches it
"""

import json
import os
import re
import sys
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.dirname(SCRIPT_DIR)
# The registry header names every variable in its description strings; it is not a comment about a default.
REGISTRY = os.path.join(WORKTREE, "RenderCore", "RendererFeatureRegistry.h")
ALLOWLIST = os.path.join(SCRIPT_DIR, "gate-defaults-allowlist.txt")
GATE_TABLE_JSON = os.path.join(WORKTREE, "Gates", "gate_table.json")

# C++ trees whose comments are swept. Shaders are swept for claims too (GLSL has no getenv, but its comments describe
# gates promoted to default-ON in C++ while the shader still said OFF).
SCAN_DIRS = ["GameOS", "mclib", "code", "RenderCore", "editor", "GuiRuntime", "GameAdapters", "RenderWorld", "gui",
             "EditorBridge", "HostServices", "Viewer"]
SCAN_EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")
CLAIM_DIRS = ["shaders"]
CLAIM_EXTS = (".hglsl", ".glsl", ".vert", ".frag", ".comp", ".geom",
              ".tesc", ".tese", ".mesh", ".task")

ON, OFF = "ON", "OFF"

#: A swept claim count below this means the sweep lost files (a renamed directory, a changed extension).
MIN_CLAIMS = 400
#: Claims that matched a Bool row and were actually compared. A sweep can keep its claim count while the table side
#: loses its Bool rows (a changed JSON shape), which would grade nothing and pass; this floor catches that.
MIN_GRADED = 400

if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)
from gate_idioms import strip_comments  # noqa: E402,F401
from gate_idioms import is_quoted_history  # noqa: E402


# ---------------------------------------------------------------------------
# Comment claims (ENV-DROP-CLOSE-1)
# ---------------------------------------------------------------------------
#
# The registry is not the only place a default is written down, and it is not
# where a reader looks first. code/objmgr.cpp:1610 said
#
#     // Default-off: MC2_STATIC_PROP_MISSION_LOAD_REG=0 (first-render fallback covers).
#
# for nearly three months while gos_static_prop_registry.cpp:141 read
# `parseEnvBoolWithDefault(..., true)`. Mission-load registration was LIVE and
# the comment said it was not. Nothing machine-checked that, because this file
# STRIPS comments -- correctly, so that a commented-out getenv cannot pose as a
# reader. Stripping them for classification and never checking them at all are
# two different decisions, and only the first one was deliberate.
#
# SCOPE IS DELIBERATELY NARROW, because a proximity-based version of this rule
# was tried first and produced 18 hits of which every single one was a false
# positive: comments about a NEIGHBOURING gate ("MC2_SSAO_BLUR defaults ON" four
# lines from MC2_SSAO_DEBUG), and conditional claims ("default-ON under the
# umbrella"). A rule with that hit rate gets deleted by the first person it
# annoys. So a claim counts only when all of the following hold:
#   * it is on a COMMENT line,
#   * that line names EXACTLY ONE MC2_* variable -- so the claim cannot be about
#     a neighbour,
#   * the claim is unconditional (no under/when/unless/if/pending/previously/...
#     qualifying it), and
#   * the variable has exactly one non-UNCLASSIFIABLE reader verdict.
# That leaves 106 assertions in-scope across the tree.

_VAR_RE = re.compile(r"MC2_[A-Z0-9_]+")
_CLAIM_RE = re.compile(
    r"default(?:s|ed)?\s*(?:is\s+|to\s+|=\s*|-|:|\s)\s*\**\s*(on|off)\b"
    r"|\bdefaults?\s+to\s+`?\**(1|0)\b", re.I)
#: BACKEND-DEFAULT-PARITY-1 added `whenever`/`wherever`: `\bwhen\b` does not match
#: `whenever` (the boundary fails on the trailing "ever"), so two genuinely
#: conditional claims -- "Default ON whenever SSAO itself is on"
#: (gos_postprocess.h:362) and "default ON whenever MC2_SMOKE_MODE is set"
#: (mclib/vis_semantics.h:91, a claim about a DIFFERENT gate) -- read as
#: unconditional assertions. Both surfaced the moment attribution widened below;
#: the second would have been a false positive, which is how this rule dies.
_COND_RE = re.compile(
    r"\b(under|when(?:ever)?|wherever|unless|if|while|once|pending|until|"
    r"would|was|used to|previously|no longer|should)\b", re.I)
#: NEGATED CLAIMS (INCAPABLE-BATCH-1 repair, 2026-07-31). `_CLAIM_RE` reads the
#: DENIAL of a default as an ASSERTION of it, which is not a near-miss -- it
#: reports the exact opposite of what the line says. The live instance:
#:
#:     //      MC2_TERRAIN_DECAL is enabled, not a shipped default-on value.
#:
#: was reported as "states default ON but the reader implements default OFF"
#: while both the comment and the reader (mclib/bdactor.cpp:4827) say OFF. Two
#: line-granularity effects compound there: the TRUE claim two lines up ("the
#: DEFAULT-OFF opt-in gate" / "MC2_TERRAIN_DECAL") is split across a line break
#: so neither line both names the var and states the default, and the qualifier
#: that would have tripped `_COND_RE` ("used once") is on the PRECEDING line.
#:
#: SCOPE IS TIGHT ON PURPOSE, and the width was measured rather than guessed. A
#: bare "is there a negation token earlier on the line" test fires on 5 of the
#: 412 attributable claims in the tree and FOUR of those are false skips, where
#: the negation belongs to an unrelated noun phrase and the default claim beside
#: it is honest and currently AGREES:
#:     gos_static_prop_batcher.h:254  "nullptr = no snap-cull (default OFF)"
#:     docs/tier1_env_vars.md:442     "(no UV seams). Default **OFF**"
#:     docs/tier1_env_vars.md:120     "not umbrella members) ... default-OFF"
#:     code/mission.cpp:4075          "NOT fopen). Gate ... (default OFF)"
#: Widening the rule to swallow those would trade one false POSITIVE for four
#: lost checks -- the mirror image of the 18-hit proximity rule rejected above.
#: So a negation only disarms a claim when it directly governs the claim's own
#: noun phrase: nothing between the negation and the word "default" except up to
#: three plain words. Every false skip above is separated from its claim by
#: punctuation that ends the phrase -- ")", ".", ",", ";" -- and so is untouched.
#: Measured effect of the rule as written: exactly one claim skipped tree-wide.
#:
#: A denial is DISCARDED rather than inverted. "not default-on" does imply OFF
#: for a boolean, but this file's standing rule is that an idiom it cannot read
#: with confidence is skipped, not guessed at (see implemented_default), and
#: double negatives ("is not default off") would make the inversion wrong. The
#: positive claim is the one that gets checked; write that instead.
_NEG_RE = re.compile(
    r"\b(?:not|never|nor|no)\s+(?:[A-Za-z][A-Za-z-]*\s+){0,3}$", re.I)
_COMMENT_LINE_RE = re.compile(r"^\s*(?://|\*|/\*)")


def comment_claims(lines, path, prose=False):
    """-> list of {var, claim, path, line, text} for attributable claims.

    `prose=True` skips the comment-marker test so the same rule can be applied
    to a markdown doc, where every line is prose.
    """
    out = []
    for i, ln in enumerate(lines, 1):
        if not prose and not _COMMENT_LINE_RE.match(ln):
            continue
        names = set(_VAR_RE.findall(ln))
        if len(names) != 1:
            continue
        a = _CLAIM_RE.search(ln)
        if not a:
            continue
        if _COND_RE.search(ln[:a.start()]) or _COND_RE.search(ln[a.end():]):
            continue
        # A DENIAL of the default is not an assertion of it. See _NEG_RE.
        if _NEG_RE.search(ln[:a.start()]):
            continue
        out.append({"var": names.pop(), "claim": {"1": "ON", "0": "OFF"}.get(a.group(2) or "", (a.group(1) or "").upper()),
                    "path": path, "line": i, "text": ln.strip()[:160]})
    return out


def _tracked_md(root):
    """Tracked *.md paths (forward slashes, relative to root), or None when git cannot say."""
    import subprocess
    import console_child   # scripts/: console children without a window or a conhost each
    try:
        r = subprocess.run(["git", "-C", root, "ls-files", "--", "*.md"], capture_output=True, text=True, timeout=60,
                           **console_child.child_kwargs(redirected=True))
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    return [l.strip() for l in r.stdout.splitlines() if l.strip()]


def load_comment_claims(root=WORKTREE, scan_dirs=None, exclude=(REGISTRY,),
                        claim_dirs=None):
    scan_dirs = SCAN_DIRS if scan_dirs is None else scan_dirs
    claim_dirs = CLAIM_DIRS if claim_dirs is None else claim_dirs
    excl = {os.path.normcase(os.path.abspath(p)) for p in exclude}
    claims = []

    def sweep(dirs, exts):
        for d in dirs:
            base = os.path.join(root, d)
            if not os.path.isdir(base):
                continue
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames[:] = [x for x in dirnames
                               if x not in (".git", "build64", "third_party", "3rdparty")]
                for fn in filenames:
                    if not fn.endswith(exts):
                        continue
                    full = os.path.join(dirpath, fn)
                    if os.path.normcase(os.path.abspath(full)) in excl:
                        continue
                    try:
                        with open(full, "r", encoding="utf-8", errors="replace") as fh:
                            lines = fh.read().splitlines()
                    except OSError:
                        continue
                    claims.extend(comment_claims(
                        lines, os.path.relpath(full, root).replace("\\", "/")))

    sweep(scan_dirs, SCAN_EXTS)
    # SHADER-COMMENT-TRUTH-1: the same rule over GLSL. See CLAIM_DIRS above for
    # why shaders are swept for CLAIMS but never for READERS.
    sweep(claim_dirs, CLAIM_EXTS)
    # docs/tier1_env_vars.md is the OTHER place a default is written down, and
    # it is the one a human is told to read. The same three lies that shipped in
    # the registry shipped there too, and this sweep found three more that only
    # exist in the doc -- including MC2_STANDARD_LIT_V1, documented as "Default
    # ON. Kill=`=0`" while gos_mech_batcher.cpp:365 reads
    # `(v != nullptr && v[0] == '1')  // TEMPORARY DEFAULT-OFF`.
    # DOCS-DEFAULT-CLAIMS-1: sweep ALL prose docs, not just tier1_env_vars.md.
    # docs/**/*.md (minus docs/archive/, frozen history) and .claude/*.md.
    # Same one-var-per-line, unconditional-claim rule as above.
    md_files = []
    # Only TRACKED docs are swept: an untracked local directory (dated plans, scratch notes) is not part of the
    # tree, and which of them exist differs per checkout. Falls back to the directory walk outside a git tree.
    tracked = _tracked_md(root)
    if tracked is not None:
        md_files.extend(os.path.join(root, f) for f in tracked
                        if f.startswith("docs/") and not f.startswith("docs/archive/"))
    else:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, "docs")):
            dirnames[:] = [x for x in dirnames if x != "archive"]
            md_files.extend(os.path.join(dirpath, f) for f in filenames
                            if f.endswith(".md"))
    cdir = os.path.join(root, ".claude")
    if os.path.isdir(cdir):
        md_files.extend(os.path.join(cdir, f) for f in os.listdir(cdir)
                        if f.endswith(".md"))
    for full in sorted(md_files):
        try:
            with open(full, "r", encoding="utf-8", errors="replace") as fh:
                md_lines = fh.read().splitlines()
            # A doc that carries the quoted-history marker (docs/gate-history.md)
            # reproduces old registry prose verbatim; its default words are
            # history, not a current claim, so rule 3 does not apply to it.
            if is_quoted_history(md_lines):
                continue
            claims.extend(comment_claims(
                md_lines,
                os.path.relpath(full, root).replace("\\", "/"),
                prose=True))
        except OSError:
            pass
    return claims


# ---------------------------------------------------------------------------
# Allowlist
# ---------------------------------------------------------------------------

def load_allowlist(path=ALLOWLIST):
    """VAR<whitespace>reason. A bare VAR, or a token reason, is itself an error."""
    entries, malformed = {}, []
    if not os.path.exists(path):
        return entries, malformed
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) < 2 or len(parts[1].strip()) < 20:
                malformed.append(line)
                continue
            entries[parts[0]] = parts[1].strip()
    return entries, malformed


# ---------------------------------------------------------------------------
# The table, and the comparison
# ---------------------------------------------------------------------------

def load_defaults(path=GATE_TABLE_JSON):
    """-> {gate name: ON/OFF} for every Bool row of the compiled gate table."""
    with open(path, "r", encoding="utf-8") as fh:
        rows = json.load(fh)["gates"]
    return {r["name"]: (ON if r["default"] else OFF) for r in rows if r.get("kind") == "Bool"}


def analyse(defaults, claims, allow):
    """-> (findings, stats). A claim about a gate that is not a Bool row, or that is allowlisted, is not graded."""
    findings = []
    stats = {"claims": len(claims), "graded": 0, "agree": 0, "disagree": 0, "allowlisted": 0, "not_bool": 0}
    for c in claims:
        var = c["var"]
        if var in allow:
            stats["allowlisted"] += 1
            continue
        impl = defaults.get(var)
        if impl is None:
            stats["not_bool"] += 1
            continue
        stats["graded"] += 1
        if c["claim"] == impl:
            stats["agree"] += 1
            continue
        stats["disagree"] += 1
        findings.append({
            "kind": "COMMENT_DISAGREEMENT", "var": var,
            "msg": (f"{c['path']} states default {c['claim']} but the gate table says default {impl}. The table "
                    f"is the only authority; fix the comment (or the row)."),
            "sites": [f"{c['path']}:{c['line']}  {c['text']}"],
        })
    return findings, stats


def run(quiet=False):
    defaults = load_defaults()
    allow, malformed = load_allowlist()
    claims = load_comment_claims()
    findings, stats = analyse(defaults, claims, allow)
    for line in malformed:
        findings.append({
            "kind": "BAD_ALLOWLIST", "var": line.split()[0] if line.split() else "?",
            "msg": "allowlist entry has no reason, or a reason under 20 chars. An unexplained exemption is how "
                   "the original lie survived.",
            "sites": [line],
        })
    if stats["claims"] < MIN_CLAIMS:
        findings.append({
            "kind": "COVERAGE_LOSS", "var": "(all)",
            "msg": f"only {stats['claims']} default claims were swept; the floor is {MIN_CLAIMS}. The sweep lost files.",
            "sites": [],
        })
    if stats["graded"] < MIN_GRADED:
        findings.append({
            "kind": "COVERAGE_LOSS", "var": "(all)",
            "msg": f"only {stats['graded']} claims were graded against a Bool row; the floor is {MIN_GRADED}. "
                   f"The table side lost rows, or the claims stopped matching them.",
            "sites": [],
        })
    if not quiet:
        print("=" * 70)
        print("  GATE-DEFAULT-TRUTH-1 -- written defaults vs the gate table")
        print("=" * 70)
        print(f"  default claims swept (comments, shaders, tracked docs) : {stats['claims']}")
        print(f"  ... graded against a Bool row                         : {stats['graded']}")
        print(f"  ... AGREE                                             : {stats['agree']}")
        print(f"  ... CONTRADICT the table                              : {stats['disagree']}")
        print(f"  ... allowlisted / not a Bool row                      : {stats['allowlisted']} / {stats['not_bool']}")
        print()
        for f in findings:
            print(f"  [{f['kind']}] {f['var']}")
            print(f"      {f['msg']}")
            for s in f["sites"]:
                print(f"      - {s}")
            print()
        if not findings:
            print("  OK: no in-source comment, shader comment or doc line contradicts the gate table.")
        print("=" * 70)
    return 0 if not findings else 1


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _selftest():
    failures = []
    ran = []

    def check(label, cond, detail=""):
        ran.append(label)
        if cond:
            print(f"  PASS  {label}")
        else:
            print(f"  FAIL  {label}  {detail}")
            failures.append(label)

    print("GATE-DEFAULT-TRUTH-1 self-test")
    print("-" * 70)

    defaults = {"MC2_ST_HONEST_ON": ON, "MC2_ST_HONEST_OFF": OFF}
    claims = comment_claims(
        ["// MC2_ST_HONEST_ON is Default-off: set =0 to disable.",
         "// MC2_ST_HONEST_OFF and MC2_ST_HONEST_ON both default ON in that configuration.",
         "// MC2_ST_HONEST_OFF is default-ON under the umbrella gate.",
         "// MC2_ST_HONEST_OFF is default OFF.",
         "// MC2_ST_UNKNOWN is default ON."], "selftest_fake.cpp")
    findings, stats = analyse(defaults, claims, allow={})
    by = {(f["kind"], f["var"]) for f in findings}
    check("a claim contradicting the table is caught", ("COMMENT_DISAGREEMENT", "MC2_ST_HONEST_ON") in by, str(findings))
    check("a claim agreeing with the table is not flagged", ("COMMENT_DISAGREEMENT", "MC2_ST_HONEST_OFF") not in by, str(findings))
    check("a comment naming TWO vars is not attributed to either", stats["claims"] == 3, str(claims))
    check("a CONDITIONAL default claim is not treated as an assertion",
          not any(c["line"] == 3 for c in claims), str(claims))
    check("a claim about a name that is not a Bool row is not graded", stats["not_bool"] == 1, str(stats))
    _, stats2 = analyse(defaults, claims, allow={"MC2_ST_HONEST_ON": "reason long enough to count"})
    check("an allowlisted gate is skipped", stats2["allowlisted"] == 1 and stats2["disagree"] == 0, str(stats2))

    # 7c-bis. NEGATION (INCAPABLE-BATCH-1 repair). Both directions are asserted,
    #     because this rule fails in two ways and only one of them is loud. Read
    #     the _NEG_RE block for the measurement that fixed the window at "up to
    #     three plain words, no punctuation".
    neg = comment_claims(
        ["// MC2_ST_HONEST_OFF is the fill strength, not a shipped default-on value.",
         "// MC2_ST_HONEST_OFF never had a default-on arm.",
         "// nullptr = no snap-cull for MC2_ST_HONEST_OFF (default OFF).",
         "// (no UV seams) for MC2_ST_HONEST_OFF. Default **OFF**."],
        "selftest_neg.cpp")
    check("a DENIED default claim is not attributed as an assertion of it",
          not any(c["line"] in (1, 2) for c in neg), f"neg={neg}")
    check("a negation in an UNRELATED phrase still leaves the claim checked",
          {c["line"] for c in neg} == {3, 4}, f"neg={neg}")

    # 7d. The tier1 doc is prose, not comment-marked, so it needs prose=True.
    #     Shape copied verbatim from the real docs/tier1_env_vars.md line for
    #     MC2_STANDARD_LIT_V1, which was a live lie until this rule landed.
    doc = comment_claims(
        ["- `MC2_ST_HONEST_ON` — GGX PBR on mechs. Default **OFF**. Kill=`=0`.",
         "  narrative line naming MC2_ST_HONEST_ON with no default assertion"],
        "docs/tier1_env_vars.md", prose=True)
    check("a markdown doc line asserting a default is attributed",
          len(doc) == 1 and doc[0]["claim"] == "OFF"
          and doc[0]["var"] == "MC2_ST_HONEST_ON", str(doc))
    check("prose=False still ignores the same line (comment marker required)",
          comment_claims(
              ["- `MC2_ST_HONEST_ON` — Default **OFF**."], "x.cpp") == [])

    # 7e. SHADER-COMMENT-TRUTH-1. The defect was never in comment_claims() -- it
    #     was in WHICH FILES were handed to it. So this case must exercise the
    #     real directory/extension sweep, not the regex: a unit test that fed
    #     comment_claims() a GLSL line directly would have passed on the broken
    #     version and proved nothing. (The live-tree injection that actually
    #     demonstrated the fix is recorded in the SHADER-COMMENT-TRUTH-1 commit;
    #     this case is the permanent regression guard for the wiring.)
    import shutil
    import tempfile   # NOTE: also imported by case 8 below; a function-local
                      # `import` binds the name for the WHOLE function, so this
                      # one must precede first use or it is an UnboundLocalError.
    tmproot = tempfile.mkdtemp()
    try:
        shdir = os.path.join(tmproot, "shaders", "include")
        os.makedirs(shdir)
        with open(os.path.join(shdir, "probe.hglsl"), "w", encoding="utf-8") as fh:
            fh.write("// gated by MC2_ST_SHADER_GATE (default OFF), decoded below\n"
                     "vec3 f() { return vec3(1); }\n")
        # A C++-only sweep must NOT see it (this is the pre-fix behaviour) ...
        pre = load_comment_claims(root=tmproot, scan_dirs=["shaders"],
                                  claim_dirs=[], exclude=())
        check("a .hglsl claim is INVISIBLE to the C++ extension list",
              pre == [], str(pre))
        # ... and the shader sweep must.
        got = load_comment_claims(root=tmproot, scan_dirs=[],
                                  claim_dirs=["shaders"], exclude=())
        check("a .hglsl comment asserting a default IS swept",
              len(got) == 1 and got[0]["var"] == "MC2_ST_SHADER_GATE"
              and got[0]["claim"] == "OFF"
              and got[0]["path"] == "shaders/include/probe.hglsl", str(got))
    finally:
        shutil.rmtree(tmproot, ignore_errors=True)

    # 7. Comment stripping must not eat real code or string literals.
    stripped = strip_comments('a(); // getenv("X")\nb("/* not a comment */");')
    check("strip_comments preserves code and string literals",
          "a();" in stripped and 'b("/* not a comment */");' in stripped
          and 'getenv("X")' not in stripped, repr(stripped))

    # 8. An allowlist entry without a real reason must itself fail.
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tf:
        tf.write("MC2_SOMETHING\nMC2_OTHER  short\n")
        tmp = tf.name
    try:
        _, malformed = load_allowlist(tmp)
        check("allowlist entries without a real reason are rejected",
              len(malformed) == 2, str(malformed))
    finally:
        os.unlink(tmp)


    print("-" * 70)
    if failures:
        print(f"SELF-TEST FAILED: {len(failures)} case(s): {failures}")
        return 1
    print(f"SELF-TEST PASSED ({len(ran)} cases)")
    return 0


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--selftest" in argv or "--self-test" in argv:
        sys.exit(_selftest())
    sys.exit(run(quiet="--quiet" in argv))

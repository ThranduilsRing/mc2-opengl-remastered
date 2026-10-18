#!/usr/bin/env python3
"""check-counter-honesty.py -- COUNTER-HONESTY-1 static guard.

WHY THIS EXISTS
---------------
A diagnostic counter that reports a confident number it cannot know is worse
than no counter: it manufactures false conclusions that cost hours.

Two real instances motivated this check.

  (1) STRUCTURALLY BLIND INPUT.  `RecipeRange::population` defaults to 0xFF
      ("never tagged").  For a long time the ONLY callers of
      setRecipePopulation() were the two BldgAppearance sites, so every tree
      and generic recipe stayed untagged and the registry's population
      breakdown could only ever report buildings.  Nothing warned.

  (2) SAMPLE READ AS CENSUS.  The [TREE_DIAG] per-instance records are capped
      at the first N per path per process.  The lines carried no marker saying
      so, so counting them in a log looked like counting submissions.

Both are the same class: an instrument whose output cannot be distinguished
from a real measurement.  This script makes new instances of the class fail
the build's contract suite instead of shipping silently.

RULES
-----
R1  POPULATION-TAG PAIRING.  Every call site that mints a static-prop recipe
    (`GameAdapters::StaticProp::syncStaticProp(`) must be followed, within
    R1_WINDOW lines, by a `setRecipePopulation(` call.  An untagged recipe is
    invisible to every population-filtered consumer AND silently corrupts the
    census.  Anchors on symbols + a pairing window, never on line numbers.

R2  SAMPLE SELF-DECLARATION.  Any first-N print limiter (`s_<name>Printed < N`
    and friends) must emit a self-declaring token -- `sample=`, `sampled`,
    `first N`, `SAMPLE` -- inside the guarded block, so a reader of the log
    cannot mistake a truncated stream for a total.  Pre-existing sites are
    grandfathered via ALLOWLIST below; the rule is a RATCHET -- new limiters
    must self-declare or be consciously allowlisted with a reason.

R3  POPULATION-VALUE COVERAGE (TREE-POP-TAG-1).  R1 asks "does this mint tag
    something?".  It cannot catch the dual failure: an enum VALUE that no mint
    anywhere ever assigns.  The [OBJBATCHER v1] summary prints one
    reg_recipes_<pop> field per GpuStaticPropPopulation enumerator, so an
    unassigned enumerator ships a field that is structurally incapable of
    being non-zero -- a permanent 0 indistinguishable from a real count of
    zero.  That is precisely the misread that cost a P0 investigation hours
    ("submit_trees=0" -> "trees are culled").  R3 requires every enumerator to
    have at least one real assignment site, or an entry in the sidecar
    inventory stating why it cannot.  It ratchets BOTH ways: a stale entry for
    an enumerator that has since gained an assignment site also fails.

    Note R3 is the STATIC counterpart to the runtime `reg_pops_ever_tagged`
    field.  The runtime marker needs a launch and a scene that exercises the
    path; R3 answers the same question from source, in the contract suite.

Usage:
    py -3 scripts/check-counter-honesty.py [--quiet]
    py -3 scripts/check-counter-honesty.py --self-test
    py -3 scripts/check-counter-honesty.py --list-limiters   # audit aid
    py -3 scripts/check-counter-honesty.py --write           # regen R3 inventory

Exit: 0 pass / 1 violation / 2 harness error.
"""

import argparse
import os
import re
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)

# --------------------------------------------------------------------------
# R1 -- recipe minting must tag population
# --------------------------------------------------------------------------
R1_MINT = re.compile(r"StaticProp::syncStaticProp\s*\(")
R1_TAG = re.compile(r"setRecipePopulation\s*\(")
R1_WINDOW = 80          # lines after the mint call to look for the tag
R1_SCAN_DIRS = ["mclib", "code", "editor", "GameAdapters"]

# --------------------------------------------------------------------------
# R2 -- first-N print limiters must say they are a sample
# --------------------------------------------------------------------------
# The house idiom, in the shapes actually used in this tree:
#   static int s_dynPrinted = 0;  if (trace && s_dynPrinted < 8) { ++s_dynPrinted; LOG(); }
#   if (s_traceCount < 8)         if (s_mismatchCount++ < kMaxMismatchPrints)
#   if (s_seenMulti.size() < 100) if (g_execFailures <= kMaxFailureLog)
# A file-scope/static counter (s_/g_ prefix, the convention here) compared against
# a small literal or a k-constant. Precision filters below require the match to sit
# in a conditional AND to guard an actual emitter, so ordinary loop bounds are out.
R2_LIMITER = re.compile(
    r"\b([sg]_\w+)(?:\.size\(\))?\s*(?:\+\+)?\s*<=?\s*"
    r"(\d{1,4}|k[A-Z]\w*|[A-Z][A-Z_0-9]{2,})\b"
)
# `s_loc < 0` / `s_cached <= 0` are sentinel "not resolved yet" tests, not print
# caps. A cap of zero would print nothing, so zero is never a limiter.
R2_ZERO_CMP = re.compile(r"<=?\s*0\b")
# The match must live in a conditional -- rules out declarations and loop headers.
R2_IN_CONDITION = re.compile(r"\bif\s*\(|&&|\|\|")
# ...and the guarded block must actually emit something.
R2_EMITS = re.compile(
    r"printf|MC2_DIAGF|_TRACE\s*\(|_DIAG\s*\(|SP_TRACE|TREE_DIAG|TEX_HANDOFF|"
    r"std::cerr|OutputDebugString"
)
# Tokens that count as the instrument declaring its own truncation.
R2_DECLARES = re.compile(
    r"sample\s*=|sampled|SAMPLE|first\s*%?\d*[dN]?\s*(?:of|records)|"
    r"SAMPLE-CAP|truncat|not a census",
    re.IGNORECASE,
)
R2_BLOCK = 30           # lines after the limiter that form its emission block
# The declaration must appear in an EMITTED STRING, not anywhere in the source.
# Without this, a constant named stripping the real `sample=first%d` token out
# of [TREE_DIAG] left R2 green, because the constant named `kTreeDiagSample`
# contains "Sample" and the search was case-insensitive over raw source. A rule an
# identifier can satisfy does not make any log line honest.
R2_STRING_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
R2_SCAN_DIRS = ["GameOS", "mclib", "code", "RenderCore", "RenderWorld",
                "GameAdapters", "editor"]

# Grandfathered R2 sites live in a sidecar inventory, one "<path>::<limiter>" key
# per line (# comments group them by family).  The file is not a dumping ground --
# it is the AUDIT RECORD of every sampled instrument this repo currently ships
# without a self-declaring output.  Read it before trusting any capped trace.
# ADDING TO IT IS A CONSCIOUS ACT.  Prefer fixing the instrument.
R2_ALLOWLIST_PATH = os.path.join(SCRIPT_DIR, "counter-honesty-allowlist.txt")

# --------------------------------------------------------------------------
# R3 -- every population enum VALUE must have an assignment site (or a reason)
# --------------------------------------------------------------------------
R3_ENUM_FILE = os.path.join("GameOS", "gameos", "gos_static_prop_batcher.h")
R3_ENUM_NAME = "GpuStaticPropPopulation"
R3_ENUM_BODY = re.compile(
    r"enum\s+class\s+" + R3_ENUM_NAME + r"\b[^{]*\{(?P<body>[^}]*)\}", re.S)
# An enumerator is the leading identifier of a comma-separated entry, with or
# without an `= N` initialiser.
R3_ENUMERATOR = re.compile(r"^\s*([A-Za-z_]\w*)\s*(?:=|$)")
# Assignment SITE = a setRecipePopulation( call. The value may sit on a later
# line (the house style wraps these), so read a small window forward and take
# the FIRST qualified enumerator in it -- that is the argument.
R3_ASSIGN_CALL = re.compile(r"setRecipePopulation\s*\(")
R3_ASSIGN_VALUE = re.compile(R3_ENUM_NAME + r"::(\w+)")
R3_ASSIGN_WINDOW = 4
R3_SCAN_DIRS = ["mclib", "code", "editor", "GameAdapters", "GameOS",
                "RenderWorld", "RenderCore"]
# Sidecar inventory of enumerators no site assigns. Same contract as the R2
# allowlist: an AUDIT RECORD, not permission. See the file's own header.
R3_INVENTORY_PATH = os.path.join(SCRIPT_DIR,
                                 "counter-honesty-population-inventory.txt")
R3_NEEDS_REASON = "<REASON REQUIRED -- state why no site can assign this>"


def load_allowlist():
    keys = set()
    if not os.path.isfile(R2_ALLOWLIST_PATH):
        return keys
    with open(R2_ALLOWLIST_PATH, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if line:
                keys.add(line)
    return keys

SRC_EXT = (".cpp", ".h", ".inl", ".hpp", ".cc")


def repo_files(dirs):
    out = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames
                           if x not in (".git", "build64", "3rdparty")]
            for fn in filenames:
                if fn.endswith(SRC_EXT):
                    out.append(os.path.join(dirpath, fn))
    return sorted(out)


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def rel(path):
    # Self-test fixtures live in the system temp dir, which on Windows may be on
    # a different drive than ROOT -- relpath raises there. Fall back to the raw
    # path rather than failing the harness.
    try:
        return os.path.relpath(path, ROOT).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


def strip_comments(lines):
    """Drop // comment text so PROSE mentioning a symbol cannot satisfy a rule.

    Caught by the lane's own ratchet test: deleting the real
    setRecipePopulation(Tree) call left R1 green, because the explanatory comment
    directly above it contains the words "setRecipePopulation()". A guard that a
    comment can satisfy is itself a lying instrument.
    """
    out = []
    for ln in lines:
        idx = ln.find("//")
        out.append(ln if idx < 0 else ln[:idx])
    return out


def check_r1(files):
    """Every recipe mint must tag its population within R1_WINDOW lines."""
    violations = []
    for path in files:
        raw = read_lines(path)
        lines = strip_comments(raw)
        for i, line in enumerate(lines):
            if not R1_MINT.search(line):
                continue
            window = "\n".join(lines[i:i + R1_WINDOW])
            if not R1_TAG.search(window):
                violations.append(
                    "%s:%d  syncStaticProp() mints a recipe but no "
                    "setRecipePopulation() within %d lines -- the recipe keeps "
                    "the 0xFF 'never tagged' default and silently corrupts the "
                    "population census." % (rel(path), i + 1, R1_WINDOW))
    return violations


def find_limiters(files):
    """Yield (path, lineno, limiter_text, declares_bool)."""
    for path in files:
        lines = read_lines(path)
        for i, line in enumerate(lines):
            stripped = line.lstrip()
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            m = R2_LIMITER.search(line)
            if not m:
                continue
            if not R2_IN_CONDITION.search(line):
                continue
            if R2_ZERO_CMP.search(m.group(0)):
                continue
            block = "\n".join(lines[i:i + R2_BLOCK])
            if not R2_EMITS.search(block):
                continue
            emitted = " ".join(R2_STRING_LIT.findall(block))
            yield (path, i + 1, m.group(0).strip(),
                   bool(R2_DECLARES.search(emitted)))


def check_r2(files, allowlist=None):
    if allowlist is None:
        allowlist = load_allowlist()
    violations = []
    for path, lineno, text, declares in find_limiters(files):
        if declares:
            continue
        key = "%s::%s" % (rel(path), text)
        if key in allowlist:
            continue
        violations.append(
            "%s:%d  first-N print limiter `%s` emits no self-declaring token "
            "(sample= / sampled / SAMPLE-CAP). A truncated stream that does "
            "not say it is truncated reads as a census. Add the token, or "
            "allowlist `%s` in scripts/check-counter-honesty.py with a reason."
            % (rel(path), lineno, text, key))
    return violations


def parse_population_enumerators(header_path):
    """Enumerator names of the population enum, in declaration order.

    Raises on a parse miss: silently returning an empty list would make R3 a
    rule that always passes -- the exact species of instrument this script
    exists to outlaw. A harness error (exit 2) is the honest outcome.
    """
    if not os.path.isfile(header_path):
        raise RuntimeError("R3: enum header not found: %s" % header_path)
    text = open(header_path, "r", encoding="utf-8", errors="replace").read()
    m = R3_ENUM_BODY.search(text)
    if not m:
        raise RuntimeError("R3: could not locate `enum class %s` in %s -- the "
                           "rule cannot verify coverage it cannot enumerate."
                           % (R3_ENUM_NAME, rel(header_path)))
    names = []
    for entry in m.group("body").split(","):
        entry = entry.split("//", 1)[0]
        em = R3_ENUMERATOR.match(entry)
        if em:
            names.append(em.group(1))
    if not names:
        raise RuntimeError("R3: `enum class %s` parsed to zero enumerators."
                           % R3_ENUM_NAME)
    return names


def find_population_assignments(files):
    """Map enumerator -> [ "path:line", ... ] for every real assignment site.

    Comments are stripped first, for the R1 reason: a rule that prose can
    satisfy is itself a lying instrument.
    """
    assigned = {}
    for path in files:
        lines = strip_comments(read_lines(path))
        for i, line in enumerate(lines):
            if not R3_ASSIGN_CALL.search(line):
                continue
            window = "\n".join(lines[i:i + R3_ASSIGN_WINDOW])
            vm = R3_ASSIGN_VALUE.search(window)
            if not vm:
                continue
            assigned.setdefault(vm.group(1), []).append(
                "%s:%d" % (rel(path), i + 1))
    return assigned


def load_population_inventory(path=None):
    """Ordered [(enumerator, reason)] from the sidecar inventory."""
    path = path or R3_INVENTORY_PATH
    entries = []
    if not os.path.isfile(path):
        return entries
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            name, _, reason = line.partition("#")
            entries.append((name.strip(), reason.strip()))
    return entries


def render_population_inventory(unassigned, existing):
    """Inventory text for `unassigned`, carrying over known reason text."""
    known = dict(existing)
    head = []
    if os.path.isfile(R3_INVENTORY_PATH):
        with open(R3_INVENTORY_PATH, "r", encoding="utf-8",
                  errors="replace") as fh:
            for raw in fh:
                if raw.lstrip().startswith("#") or not raw.strip():
                    head.append(raw.rstrip("\n"))
                else:
                    break
    body = ["%s  # %s" % (n, known.get(n) or R3_NEEDS_REASON)
            for n in unassigned]
    return "\n".join(head + body) + "\n"


def check_r3(files, header_path=None, inventory_path=None):
    """Every population enumerator needs an assignment site, or a reason."""
    header_path = header_path or os.path.join(ROOT, R3_ENUM_FILE)
    enumerators = parse_population_enumerators(header_path)
    assigned = find_population_assignments(files)
    inventory = load_population_inventory(inventory_path)
    inv_names = [n for n, _ in inventory]

    violations = []
    for name in enumerators:
        if name in assigned:
            continue
        if name in inv_names:
            continue
        violations.append(
            "%s::%s is never assigned by any setRecipePopulation() call site. "
            "Every recipe therefore keeps the 0xFF 'never tagged' default for "
            "this value, so the [OBJBATCHER v1] reg_recipes_* field for it can "
            "only ever print 0 -- a zero no code path could have moved, which "
            "reads exactly like a real count of zero. Wire an assignment site, "
            "or record why none can exist in %s (regen: --write)."
            % (R3_ENUM_NAME, name, rel(R3_INVENTORY_PATH)))

    # Ratchet the other way: an inventory entry that has since gained a real
    # assignment site is a stale over-claim of blindness. Left unchecked, the
    # file would keep excusing a field that is now genuinely measured.
    for name, _reason in inventory:
        if name in assigned:
            violations.append(
                "%s lists %s::%s as never-assigned, but it IS assigned at %s. "
                "Stale inventory entry -- drop it (regen: --write)."
                % (rel(R3_INVENTORY_PATH), R3_ENUM_NAME, name,
                   ", ".join(assigned[name][:3])))
        elif name not in enumerators:
            violations.append(
                "%s lists %s, which is not an enumerator of %s. Stale entry."
                % (rel(R3_INVENTORY_PATH), name, R3_ENUM_NAME))

    # A reasonless entry is an entry that explains nothing. The whole point of
    # the sidecar is the reason text; an empty one silently becomes a mute
    # suppression list.
    for name, reason in inventory:
        if not reason or reason == R3_NEEDS_REASON:
            violations.append(
                "%s entry `%s` carries no reason. State why no site can assign "
                "it -- an unexplained suppression is how the original bug hid."
                % (rel(R3_INVENTORY_PATH), name))
    return violations


def write_population_inventory():
    """Regenerate the R3 inventory. newline='\\n' so a Windows regen with
    identical content does not dirty the tracked file with CRLF."""
    enumerators = parse_population_enumerators(os.path.join(ROOT, R3_ENUM_FILE))
    assigned = find_population_assignments(repo_files(R3_SCAN_DIRS))
    unassigned = [n for n in enumerators if n not in assigned]
    text = render_population_inventory(unassigned,
                                       load_population_inventory())
    with open(R3_INVENTORY_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("counter-honesty: R3 inventory regenerated -> %s (%d never-assigned "
          "enumerator(s): %s)"
          % (rel(R3_INVENTORY_PATH), len(unassigned),
             ", ".join(unassigned) or "none"))
    return 0


# --------------------------------------------------------------------------
# Self-test: prove each rule still catches a fresh violation.
# --------------------------------------------------------------------------
R1_BAD = """\
void Foo::registerStatic() {
    int32_t regIdx = -1;
    (void)GameAdapters::StaticProp::syncStaticProp(
        shape, batch.data(), batch.size(), &regIdx);
    if (regIdx >= 0) {
        staticReg.registered = true;
    }
}
"""

# Regression fixture for the ratchet test that caught R1 red-handed: a comment
# mentioning setRecipePopulation() must NOT satisfy the pairing rule.
R1_BAD_COMMENT_ONLY = """\
void Foo::registerStatic() {
    int32_t regIdx = -1;
    (void)GameAdapters::StaticProp::syncStaticProp(
        shape, batch.data(), batch.size(), &regIdx);
    if (regIdx >= 0) {
        // Historically only BldgAppearance called setRecipePopulation() here.
        staticReg.registered = true;
    }
}
"""

R1_GOOD = """\
void Foo::registerStatic() {
    int32_t regIdx = -1;
    (void)GameAdapters::StaticProp::syncStaticProp(
        shape, batch.data(), batch.size(), &regIdx);
    if (regIdx >= 0) {
        GpuStaticPropRegistry::setRecipePopulation(
            regIdx, GpuStaticPropPopulation::Tree);
    }
}
"""

R2_BAD = """\
void emit() {
    static int s_dynPrinted = 0;
    if (s_trace && s_dynPrinted < 8) {
        ++s_dynPrinted;
        fprintf(stderr, "[THING] typeID=%u\\n", id);
    }
}
"""

R2_GOOD = """\
void emit() {
    static int s_dynPrinted = 0;
    if (s_trace && s_dynPrinted < 8) {
        ++s_dynPrinted;
        fprintf(stderr, "[THING] sample=first8 typeID=%u\\n", id);
    }
}
"""


R3_ENUM_HEADER_FIXTURE = """\
enum class GpuStaticPropPopulation : uint8_t {
    Building = 0,
    Tree     = 1,
    Generic  = 2,
};
"""

# Only Building is really assigned. Tree's ONLY mention is inside a comment --
# the R1 lesson applied to R3: prose must not satisfy the rule.
R3_SRC_PARTIAL = """\
void a() {
    GpuStaticPropRegistry::setRecipePopulation(
        regIdx, GpuStaticPropPopulation::Building);
}
void b() {
    // Historically this called setRecipePopulation(GpuStaticPropPopulation::Tree).
    staticReg.registered = true;
}
"""

R3_SRC_ALL = """\
void a() {
    GpuStaticPropRegistry::setRecipePopulation(
        regIdx, GpuStaticPropPopulation::Building);
    GpuStaticPropRegistry::setRecipePopulation(
        regIdx, GpuStaticPropPopulation::Tree);
    GpuStaticPropRegistry::setRecipePopulation(
        regIdx, GpuStaticPropPopulation::Generic);
}
"""


def _r3_case(td, tag, src, inventory_text):
    """Materialise an R3 fixture triple; return the check_r3 violations."""
    hdr = os.path.join(td, "%s_enum.h" % tag)
    with open(hdr, "w", encoding="utf-8") as fh:
        fh.write(R3_ENUM_HEADER_FIXTURE)
    src_path = os.path.join(td, "%s_src.cpp" % tag)
    with open(src_path, "w", encoding="utf-8") as fh:
        fh.write(src)
    inv = os.path.join(td, "%s_inv.txt" % tag)
    with open(inv, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(inventory_text)
    return check_r3([src_path], header_path=hdr, inventory_path=inv)


def self_test_r3():
    ok = True
    checks = [
        ("R3 must FLAG an enumerator no site assigns",
         R3_SRC_PARTIAL, "# inv\n", True),
        ("R3 must FLAG an enumerator whose only assignment is in a COMMENT",
         R3_SRC_PARTIAL, "# inv\nGeneric  # no registerStatic override\n", True),
        ("R3 must PASS when every unassigned enumerator is inventoried",
         R3_SRC_PARTIAL,
         "# inv\nTree  # fixture\nGeneric  # fixture\n", False),
        ("R3 must PASS when every enumerator has a real assignment site",
         R3_SRC_ALL, "# inv\n", False),
        ("R3 must FLAG a STALE inventory entry (now assigned)",
         R3_SRC_ALL, "# inv\nTree  # fixture\n", True),
        ("R3 must FLAG an inventory entry with no reason",
         R3_SRC_PARTIAL, "# inv\nTree\nGeneric  # fixture\n", True),
    ]
    with tempfile.TemporaryDirectory() as td:
        for idx, (label, src, inv, expect_hit) in enumerate(checks):
            got = bool(_r3_case(td, "c%d" % idx, src, inv))
            if got != expect_hit:
                print("SELF-TEST FAIL: %s (expected hit=%s, got=%s)"
                      % (label, expect_hit, got))
                ok = False
            else:
                print("SELF-TEST ok: %s" % label)
    return ok


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as td:
        cases = [
            ("r1_bad.cpp", R1_BAD, check_r1, True, "R1 must FLAG an untagged mint"),
            ("r1_good.cpp", R1_GOOD, check_r1, False, "R1 must PASS a tagged mint"),
            ("r1_comment.cpp", R1_BAD_COMMENT_ONLY, check_r1, True,
             "R1 must FLAG a mint whose only setRecipePopulation is in a COMMENT"),
            ("r2_bad.cpp", R2_BAD, lambda f: check_r2(f, allowlist=set()),
             True, "R2 must FLAG a silent limiter"),
            ("r2_good.cpp", R2_GOOD, lambda f: check_r2(f, allowlist=set()),
             False, "R2 must PASS a declaring limiter"),
        ]
        for fn, body, fn_check, expect_hit, label in cases:
            p = os.path.join(td, fn)
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(body)
            hits = fn_check([p])
            got = bool(hits)
            if got != expect_hit:
                print("SELF-TEST FAIL: %s (expected hit=%s, got=%s)"
                      % (label, expect_hit, got))
                ok = False
            else:
                print("SELF-TEST ok: %s" % label)
    if not self_test_r3():
        ok = False
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--list-limiters", action="store_true",
                    help="audit aid: print every first-N print limiter found")
    ap.add_argument("--emit-allowlist", action="store_true",
                    help="print allowlist keys for every currently-silent limiter "
                         "(regeneration aid; review before pasting)")
    ap.add_argument("--write", action="store_true",
                    help="regenerate the R3 never-assigned-population inventory "
                         "(verify-only without this flag)")
    args = ap.parse_args()

    if args.write:
        return write_population_inventory()

    if args.emit_allowlist:
        for path, lineno, text, declares in find_limiters(repo_files(R2_SCAN_DIRS)):
            if not declares:
                print("%s::%s" % (rel(path), text))
        return 0

    if args.self_test:
        return self_test()

    if args.list_limiters:
        for path, lineno, text, declares in find_limiters(repo_files(R2_SCAN_DIRS)):
            print("%-6s %s:%d  %s"
                  % ("DECL" if declares else "SILENT", rel(path), lineno, text))
        return 0

    v1 = check_r1(repo_files(R1_SCAN_DIRS))
    v2 = check_r2(repo_files(R2_SCAN_DIRS))
    v3 = check_r3(repo_files(R3_SCAN_DIRS))
    violations = v1 + v2 + v3

    if violations:
        print("counter-honesty: %d violation(s)" % len(violations))
        for v in violations:
            print("  " + v)
        return 1

    if not args.quiet:
        print("counter-honesty: R1 recipe-population pairing OK; "
              "R2 sample self-declaration OK; "
              "R3 population-value coverage OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # harness error
        print("check-counter-honesty.py: harness error: %s" % exc,
              file=sys.stderr)
        sys.exit(2)

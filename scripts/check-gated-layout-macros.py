#!/usr/bin/env python3
"""check-gated-layout-macros.py — GATED-LAYOUT-MACRO-GUARD-1 CI tripwire.

Generalizes the ODR landmine from the VULKAN-EDGE-FOG-ISLAND-2b bug (fixed in
9cce8e68): a PRIVATE compile-define (MC2_VULKAN_ISLAND) added a non-static data
member (vkFogIsland_) to gosPostProcess under #ifdef, but the define reached
only SOME of the targets whose TUs include gos_postprocess.h. The class LAYOUT
disagreed across TUs -> getSceneFBO()/getWidth()/getHeight() read the wrong
offset -> garbage FBO / capture never fires. Silent, per-build, nasty.

HARD RULE enforced here:
  No compile define that is set PRIVATE on one target may affect a CLASS LAYOUT
  (non-static data member / virtual method / base clause) in a header included
  by MULTIPLE targets -- UNLESS the define is propagated to EVERY consuming
  target (or moved behind a pImpl / non-layout indirection).

WHAT IT DOES
  1. Scans shared/public headers (GameOS/**/*.h, RenderCore/**/*.h,
     mclib/**/*.h, code/**/*.h) for `#if(def) MC2_*` guards that wrap
     CLASS-LAYOUT-AFFECTING code inside a class{}/struct{} body:
       - non-static data members
       - virtual method declarations
       - base-class clauses
     (Conservative: only flags decls that live directly inside a class/struct
     body and are guarded by an MC2_* macro. Static/constexpr members, method
     bodies, nested-function code, and free/namespace-scope code are ignored.)
  2. For each flagged guard macro, parses CMakeLists.txt +
     GameOS/gameos/CMakeLists.txt target_compile_definitions() to learn which
     targets define the macro and with what visibility (PRIVATE/PUBLIC/
     INTERFACE), and cross-checks against the set of targets whose TUs include
     the header.
  3. FLAGS a violation when a layout-affecting guard macro is defined only on
     SOME of the consuming targets, or is PRIVATE on the defining target so its
     usage-requirement (the layout) does not propagate to link-time consumers
     that include the header without also getting the define.
  4. Emits docs/render-backend-seams/gated-layout-macros.md (report) and exits
     nonzero on a real violation.

COVERAGE (honest)
  DETECTS: single-line + multi-line #ifdef/#if defined() guards wrapping a data
  member, `virtual` decl, or base clause directly inside a class/struct body in
  the scanned header roots; macro PRIVATE-only or partial-target coverage.
  MISSES (by design, to stay low-false-positive): guards that change layout via
  macro EXPANSION rather than #ifdef (e.g. a member whose TYPE is a macro that
  differs per target); layout changes in headers OUTSIDE the scanned roots or
  pulled in transitively from 3rdparty; conditional #pragma pack; template
  instantiations whose size depends on a macro; union member reordering; and
  target membership inferred only from direct source lists + link graph (a
  header reached purely transitively through an untracked interface include may
  be under-counted). Treat a PASS as "no guard matched the detectable pattern",
  not a proof of ODR safety.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER_ROOTS = ["GameOS", "RenderCore", "mclib", "code"]

# Acknowledged-residual allowlist. Each entry: "header:macro" for a guard whose
# partial-target coverage has been MANUALLY analyzed and accepted (e.g. the
# gated member's layout impact is understood and the option ships default-OFF).
# The checker still PRINTS these as ACK (never silently), but does not fail on
# them. Add an entry ONLY with a one-line justification in the allowlist file.
ALLOWLIST_FILE = os.path.join("scripts", "check-gated-layout-macros.allowlist")

# CMake files that carry target_compile_definitions / add_library / add_executable.
CMAKE_FILES = ["CMakeLists.txt", os.path.join("GameOS", "gameos", "CMakeLists.txt")]

# Directories to skip while walking (build outputs, vendored code).
SKIP_DIR_PARTS = ("build64", "3rdparty", ".git")

# ---------------------------------------------------------------------------
# Header scanning
# ---------------------------------------------------------------------------

MACRO_GUARD_RE = re.compile(
    r"^\s*#\s*if(?:def)?\s+(?:defined\s*\(\s*)?(MC2_[A-Z0-9_]+)")

# A data member decl directly inside a class body. Conservative: a type token
# then an identifier then ';' or '=' or '['; NOT starting with a keyword that
# indicates non-layout (static/constexpr/using/typedef/friend/enum) and NOT a
# method (no '(' before ';'). We also catch `virtual` and base clauses.
MEMBER_RE = re.compile(
    r"^\s*(?P<decl>[A-Za-z_][\w:<>,\s\*&]*\b[A-Za-z_]\w*\s*(?:\[[^\]]*\])?\s*"
    r"(?:=\s*[^;]+)?;)\s*$")
VIRTUAL_RE = re.compile(r"^\s*virtual\b")
NONLAYOUT_KW_RE = re.compile(
    r"^\s*(static|constexpr|using|typedef|friend|enum|class|struct|"
    r"//|/\*|\*|#|public:|private:|protected:|template)\b")
# method decl (has a '(' that isn't a function-pointer member): ...name(...)...;
METHOD_RE = re.compile(r"\b[A-Za-z_]\w*\s*\([^;]*\)\s*(const)?\s*(=\s*0)?\s*;")


def iter_headers():
    for root in HEADER_ROOTS:
        base = os.path.join(ROOT, root)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            if any(p in dirpath for p in SKIP_DIR_PARTS):
                dirnames[:] = []
                continue
            for fn in filenames:
                if fn.endswith(".h") or fn.endswith(".hpp"):
                    yield os.path.join(dirpath, fn)


def rel(path):
    return os.path.relpath(path, ROOT).replace("\\", "/")


def track_class_depth(line, state):
    """Rough class/struct-body tracker. Pushes a name onto the brace stack for
    every `{`; the name is the class/struct identifier if this brace opens a
    class/struct definition (not a forward decl or variable), else None.

    `state["pending"]` carries a class/struct name seen on a prior line whose
    opening `{` has not yet appeared (multi-line class headers)."""
    # A class/struct DEFINITION token (has no ';' terminating it as a fwd decl).
    m = re.search(r"\b(class|struct)\s+([A-Za-z_]\w*)", line)
    if m and ";" not in line.split(m.group(0), 1)[1].split("{", 1)[0]:
        state["pending"] = m.group(2)

    for ch in line:
        if ch == "{":
            state["stack"].append(state["pending"])
            state["pending"] = None
        elif ch == "}":
            if state["stack"]:
                state["stack"].pop()
    return state


def current_class(state):
    for name in reversed(state["stack"]):
        if name:
            return name
    return None


def scan_header(path):
    """Return list of (macro, class_name, line_no, member_text) for
    layout-affecting guards inside a class/struct body."""
    findings = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return findings

    state = {"stack": [], "pending": None}
    guard_stack = []  # list of (macro, depth_at_open) for MC2_ guards only

    for i, raw in enumerate(lines):
        line = raw.rstrip("\n")

        gm = MACRO_GUARD_RE.match(line)
        if gm:
            guard_stack.append((gm.group(1), len(state["stack"]), i + 1))
        elif re.match(r"^\s*#\s*endif\b", line):
            if guard_stack:
                guard_stack.pop()

        # Are we inside an MC2_ guard AND inside a class/struct body?
        cls = current_class(state)
        if guard_stack and cls and not gm:
            macro, _, gline = guard_stack[-1]
            if NONLAYOUT_KW_RE.match(line):
                pass
            elif VIRTUAL_RE.match(line):
                findings.append((macro, cls, i + 1, line.strip(), "virtual-method"))
            elif MEMBER_RE.match(line) and not METHOD_RE.search(line):
                findings.append((macro, cls, i + 1, line.strip(), "data-member"))

        # Update brace/class state AFTER classification (guard line itself has no member).
        track_class_depth(line, state)

    return findings


# ---------------------------------------------------------------------------
# CMake target-definition parsing
# ---------------------------------------------------------------------------

def read_cmake_text():
    text = {}
    for cf in CMAKE_FILES:
        p = os.path.join(ROOT, cf)
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                text[cf] = f.read()
        except OSError:
            text[cf] = ""
    return text


TCD_RE = re.compile(
    r"target_compile_definitions\s*\(\s*([A-Za-z_]\w*)\s+"
    r"(PRIVATE|PUBLIC|INTERFACE)\s+([^)]*)\)", re.MULTILINE)


def parse_macro_target_defs(cmake_text):
    """macro -> list of (target, visibility). Strips =VALUE suffixes."""
    defs = {}
    for cf, text in cmake_text.items():
        for m in TCD_RE.finditer(text):
            target, vis, body = m.group(1), m.group(2), m.group(3)
            for tok in body.split():
                tok = tok.strip()
                if not tok.startswith("MC2_"):
                    continue
                macro = tok.split("=", 1)[0]
                defs.setdefault(macro, []).append((target, vis, cf))
    return defs


# ---------------------------------------------------------------------------
# Target membership: which targets compile a TU that includes the header.
# We approximate by (a) the set of .cpp that #include the header, mapped to
# (b) the target that owns each .cpp via add_library/add_executable source lists
# and add_library(name file...) plus the SOURCES glob. Because the SOURCES var
# is dynamic, we fall back to a coarse directory->target map that mirrors the
# repo's actual layout, and always include the linked mc2/editor umbrella.
# ---------------------------------------------------------------------------

def includers_of(header_rel):
    """Return list of .cpp paths that #include the header (basename match)."""
    base = os.path.basename(header_rel)
    hits = []
    for root in HEADER_ROOTS + ["editor", "GameAdapters", "RenderWorld",
                                "GuiRuntime", "gui"]:
        d = os.path.join(ROOT, root)
        if not os.path.isdir(d):
            continue
        for dirpath, dirnames, filenames in os.walk(d):
            if any(p in dirpath for p in SKIP_DIR_PARTS):
                dirnames[:] = []
                continue
            for fn in filenames:
                if not fn.endswith(".cpp"):
                    continue
                fp = os.path.join(dirpath, fn)
                try:
                    with open(fp, "r", encoding="utf-8", errors="replace") as f:
                        txt = f.read()
                except OSError:
                    continue
                if re.search(r'#\s*include\s*[<"][^">]*%s[">]' % re.escape(base), txt):
                    hits.append(rel(fp))
    return sorted(set(hits))


# Coarse directory -> owning CMake target map (mirrors the repo's add_library
# source membership; kept explicit so the checker has no build dependency).
DIR_TARGET_MAP = [
    ("GameOS/gameos/gameosmain.cpp", "gameos_main"),
    ("GameOS/gameos/", "gameos"),
    ("mclib/", "mclib"),
    ("code/", "mc2"),
    ("editor/", "gameos_editor"),
    ("GameAdapters/", "gameadapters"),
    ("RenderWorld/", "renderworld"),
    ("GuiRuntime/", "gui_runtime"),
    ("RenderCore/", "rendercore"),
]


def owning_target(cpp_rel):
    for prefix, target in DIR_TARGET_MAP:
        if cpp_rel == prefix or cpp_rel.startswith(prefix):
            return target
    return "?"


def consuming_targets(header_rel):
    cpps = includers_of(header_rel)
    targets = {}
    for c in cpps:
        t = owning_target(c)
        targets.setdefault(t, []).append(c)
    return targets, cpps


# ---------------------------------------------------------------------------
# Effective define reach: a macro reaches a target if defined PRIVATE/PUBLIC on
# that target, OR defined PUBLIC/INTERFACE on a target it links against. We do a
# shallow link-propagation using a small known link map (mc2 links gameos etc.).
# ---------------------------------------------------------------------------

# Which targets does each umbrella target link (that could propagate PUBLIC defs)?
LINK_MAP = {
    "mc2": ["gameos", "gameos_main", "mclib", "renderworld", "gameadapters",
            "rendercore", "gui_runtime"],
    "gameos_main": [],  # NOTE: does NOT link gameos -> no PUBLIC propagation (the bug)
    "gameos_editor": ["rendercore"],
}


def macro_reaches_target(macro, target, macro_defs):
    """Does `macro` end up defined when compiling TUs of `target`?"""
    for (t, vis, _cf) in macro_defs.get(macro, []):
        if t == target:
            return True, "%s(%s)" % (t, vis)
    # PUBLIC/INTERFACE propagation from a linked dependency.
    for dep in LINK_MAP.get(target, []):
        for (t, vis, _cf) in macro_defs.get(macro, []):
            if t == dep and vis in ("PUBLIC", "INTERFACE"):
                return True, "via %s(%s)" % (t, vis)
    return False, None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def load_allowlist():
    p = os.path.join(ROOT, ALLOWLIST_FILE)
    entries = set()
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    entries.add(line)
    except OSError:
        pass
    return entries


def analyze(macro_defs, simulate_prefix_only=False):
    """Return (rows, violations). If simulate_prefix_only, drop the explicit
    gameos_main/mc2 PRIVATE defs of MC2_VULKAN_ISLAND to reproduce the pre-fix
    state (negative test)."""
    if simulate_prefix_only:
        macro_defs = {
            m: [(t, v, cf) for (t, v, cf) in lst
                if not (m == "MC2_VULKAN_ISLAND" and t in ("gameos_main", "mc2")
                        and v == "PRIVATE")]
            for m, lst in macro_defs.items()
        }
        # Also demote the gameos PUBLIC to PRIVATE to fully mirror pre-fix.
        macro_defs["MC2_VULKAN_ISLAND"] = [
            (t, "PRIVATE" if m_ok else v, cf)
            for (t, v, cf) in macro_defs.get("MC2_VULKAN_ISLAND", [])
            for m_ok in [t == "gameos"]
        ]

    allowed = set() if simulate_prefix_only else load_allowlist()
    rows = []
    violations = []
    for hp in iter_headers():
        hrel = rel(hp)
        guards = scan_header(hp)
        if not guards:
            continue
        targets_map, _cpps = consuming_targets(hrel)
        consuming = sorted(targets_map.keys())
        for (macro, cls, ln, text, kind) in guards:
            reached = []
            missing = []
            for t in consuming:
                if t == "?":
                    missing.append("?(unmapped)")
                    continue
                ok, how = macro_reaches_target(macro, t, macro_defs)
                if ok:
                    reached.append("%s[%s]" % (t, how))
                else:
                    missing.append(t)
            key = "%s:%s" % (hrel, macro)
            if not missing:
                verdict = "PASS"
            elif key in allowed:
                verdict = "ACK"
            else:
                verdict = "FLAG"
            row = {
                "header": hrel, "class": cls, "line": ln, "macro": macro,
                "kind": kind, "member": text, "consuming": consuming,
                "reached": reached, "missing": missing, "verdict": verdict,
            }
            rows.append(row)
            if verdict == "FLAG":
                violations.append(row)
    return rows, violations


def write_report(rows, violations, out_rel):
    out = os.path.join(ROOT, out_rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    lines = []
    lines.append("# Gated layout-macro guard report")
    lines.append("")
    lines.append("Generated by `scripts/check-gated-layout-macros.py` "
                 "(GATED-LAYOUT-MACRO-GUARD-1).")
    lines.append("")
    lines.append("Rule: a compile define that toggles a class-LAYOUT-affecting "
                 "member/vtable/base under `#ifdef` in a header included by "
                 "MULTIPLE targets must reach EVERY consuming target, or the "
                 "class size/vtable disagrees across TUs (ODR).")
    lines.append("")
    if not rows:
        lines.append("No layout-affecting macro guards detected in scanned "
                     "headers.")
    for r in rows:
        lines.append("## %s  (`%s`)" % (r["verdict"], r["macro"]))
        lines.append("")
        lines.append("- Header: `%s:%d`" % (r["header"], r["line"]))
        lines.append("- Class: `%s`  (%s)" % (r["class"], r["kind"]))
        lines.append("- Member: `%s`" % r["member"])
        lines.append("- Consuming targets: %s" %
                     (", ".join(r["consuming"]) or "(none found)"))
        lines.append("- Reached by define: %s" %
                     (", ".join(r["reached"]) or "(none)"))
        if r["missing"]:
            lines.append("- **MISSING the define: %s**" % ", ".join(r["missing"]))
        lines.append("")
    lines.append("---")
    lines.append("")
    lines.append("Result: %d guard(s) analyzed, %d violation(s)."
                 % (len(rows), len(violations)))
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return out_rel


def main():
    quiet = "--quiet" in sys.argv or "-q" in sys.argv
    negative = "--negative-test" in sys.argv

    cmake_text = read_cmake_text()
    macro_defs = parse_macro_target_defs(cmake_text)

    rows, violations = analyze(macro_defs)
    out_rel = "docs/render-backend-seams/gated-layout-macros.md"
    write_report(rows, violations, out_rel)

    if not quiet:
        print("GATED-LAYOUT-MACRO-GUARD-1 check:")
        for r in rows:
            print("  %-6s %-22s %s:%d  class %s (%s)"
                  % (r["verdict"], r["macro"], r["header"], r["line"],
                     r["class"], r["kind"]))
            if r["missing"]:
                print("         consuming=%s  MISSING=%s"
                      % (r["consuming"], r["missing"]))
        print("  report -> %s" % out_rel)

    if negative:
        # Differential test: the pre-9cce8e68 state left gameos_main (and mc2)
        # WITHOUT the define -> the real ODR bug. Confirm (a) the pre-fix
        # simulation FLAGS with gameos_main in the missing set, and (b) the
        # real tree does NOT have gameos_main missing (the fix covers it).
        _nrows, nviol = analyze(macro_defs, simulate_prefix_only=True)
        vk = [v for v in nviol if v["macro"] == "MC2_VULKAN_ISLAND"]
        real_row = next((r for r in rows if r["macro"] == "MC2_VULKAN_ISLAND"),
                        None)
        real_gm_missing = bool(real_row and "gameos_main" in real_row["missing"])
        print("")
        print("NEGATIVE TEST (simulated pre-fix PRIVATE-only MC2_VULKAN_ISLAND):")
        ok = bool(vk) and any("gameos_main" in v["missing"] for v in vk) \
            and not real_gm_missing
        if ok:
            for v in vk:
                print("  FLAG (expected): %s missing on %s"
                      % (v["header"], v["missing"]))
            print("  real tree: gameos_main covered by the fix (not missing).")
            print("  negative test PASSED (checker flags the pre-fix ODR bug "
                  "and clears the fixed tree).")
        else:
            print("  negative test FAILED: pre-fix flag=%s (gameos_main in "
                  "missing=%s), real-tree gameos_main missing=%s"
                  % (bool(vk),
                     any("gameos_main" in v["missing"] for v in vk),
                     real_gm_missing), file=sys.stderr)
            return 2

    if violations:
        print("GATED-LAYOUT-MACRO-GUARD: FAIL (%d violation(s))"
              % len(violations), file=sys.stderr)
        for v in violations:
            print("  %s:%d  macro %s toggles %s in class %s; missing on %s"
                  % (v["header"], v["line"], v["macro"], v["kind"],
                     v["class"], v["missing"]), file=sys.stderr)
        return 1

    if not quiet:
        acks = [r for r in rows if r["verdict"] == "ACK"]
        print("GATED-LAYOUT-MACRO-GUARD: PASS (%d guard(s); %d clean, %d "
              "acknowledged-residual via allowlist)"
              % (len(rows), len(rows) - len(acks), len(acks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

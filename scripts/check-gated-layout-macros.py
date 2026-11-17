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


CM_LITERAL_RE = re.compile(
    r'#?\[(=*)\[.*?\]\1\]|#[^\n]*|"(?:\\.|[^"\\])*"', re.DOTALL)


def cmake_commands(text):
    """Read statement commands, keeping comments and literal contents opaque."""
    literals = {}

    def hide(match):
        value = match.group()
        if value.startswith("#"):
            return re.sub(r"[^\n]", " ", value)
        key = "__cmake_literal_%d__" % len(literals)
        literals[key] = (value[1:-1] if value.startswith('"') else
                         value[len(match[1]) + 2:-(len(match[1]) + 2)])
        return key + "\n" * value.count("\n")

    masked = CM_LITERAL_RE.sub(hide, text)
    consumed = 0
    for match in re.finditer(r"^[ \t]*([A-Za-z_]\w*)\s*\(", masked, re.MULTILINE):
        if match.start() < consumed:
            continue
        depth = 1
        for end in range(match.end(), len(masked)):
            depth += (masked[end] == "(") - (masked[end] == ")")
            if not depth:
                break
        else:
            raise ValueError("unterminated CMake command: " + match[1])
        consumed = end + 1
        args = [literals.get(arg, arg) for arg in masked[match.end():end].split()]
        yield match[1].lower(), args, masked.count("\n", 0, match.start()) + 1


def parse_macro_target_defs(cmake_text):
    """macro -> (target, visibility, file); expand literal helper calls only.

    This remains a textual coverage approximation, not a CMake interpreter.
    Nested definitions and recursive helpers fail rather than grant coverage.
    """
    defs = {}
    helpers, commands = {}, {}
    for cf, text in cmake_text.items():
        commands[cf] = []
        definition = None
        for name, args, line in cmake_commands(text):
            if name in ("function", "macro"):
                if definition or not args:
                    raise ValueError("unsupported nested/empty CMake definition in " + cf)
                if args[0].lower() in helpers:
                    raise ValueError("unsupported CMake construct in layout coverage: "
                                     "%s:%d (duplicate helper %s)" % (cf, line, args[0]))
                definition = (name, args, [])
            elif name in ("endfunction", "endmacro"):
                if not definition or name != "end" + definition[0]:
                    raise ValueError("unbalanced CMake definition in " + cf)
                _, signature, body = definition
                helpers[signature[0].lower()] = (signature[1:], body, cf)
                definition = None
            else:
                (definition[2] if definition else commands[cf]).append((name, args, line))
        if definition:
            raise ValueError("unterminated CMake definition in " + cf)

    def expand(name, args, cf, active=()):
        if name in helpers:
            if name in active:
                raise ValueError("recursive CMake helper: " + name)
            params, body, helper_cf = helpers[name]
            for command, _, line in body:
                if command == "return":
                    raise ValueError("unsupported CMake construct in layout coverage: "
                                     "%s:%d (return in helper %s)" % (helper_cf, line, name))
            if len(params) != len(args) or not all(
                    re.fullmatch(r"[A-Za-z_][\w.-]*", arg) for arg in args):
                return
            for command, values, _ in body:
                for param, value in zip(params, args):
                    values = [v.replace("${" + param + "}", value) for v in values]
                expand(command, values, cf, active + (name,))
        elif name == "target_compile_definitions" and len(args) >= 3:
            target, vis = args[0], None
            if not re.fullmatch(r"[A-Za-z_][\w.-]*", target):
                return
            for tok in args[1:]:
                if tok in ("PRIVATE", "PUBLIC", "INTERFACE"):
                    vis = tok
                elif vis and tok.startswith("MC2_"):
                    defs.setdefault(tok.split("=", 1)[0], []).append((target, vis, cf))

    for cf, body in commands.items():
        for name, args, _ in body:
            expand(name, args, cf)
    return defs


# ---------------------------------------------------------------------------
# Target membership: which targets compile a TU that includes the header.
# We approximate by (a) the set of .cpp that #include the header, mapped to
# (b) the target that owns each .cpp via add_library/add_executable source lists
# and add_library(name file...) plus the SOURCES glob. Because the SOURCES var
# is dynamic, we fall back to a coarse directory->target map that mirrors the
# repo's actual layout, and always include the linked mc2/editor umbrella.
# ---------------------------------------------------------------------------

def _split_internal_text(dirpath, txt, depth=0):
    """Text of the `<stem>_internal.h` headers a translation unit includes from its own directory: a split unit keeps its include bundle there,
    so the units' real includes would otherwise be invisible to the textual scan."""
    out = ""
    if depth > 1:
        return out
    for m in re.finditer(r'#\s*include\s*"([\w.\-]+_internal\.h)"', txt):
        fp = os.path.join(dirpath, m.group(1))
        try:
            with open(fp, "r", encoding="utf-8", errors="replace") as f:
                inner = f.read()
        except OSError:
            continue
        out += inner + _split_internal_text(dirpath, inner, depth + 1)
    return out


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
                txt += _split_internal_text(dirpath, txt)
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


def render_report(rows, violations):
    """Build the report text. Pure -- writes nothing.

    Separated from writing so the default path can VERIFY the tracked report
    instead of rewriting it. See verify_or_write_report().
    """
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
    return "\n".join(lines) + "\n"


def verify_or_write_report(rows, violations, out_rel, write=False):
    """VERIFY the tracked report by default; regenerate only with --write.

    This checker used to rewrite `out_rel` as a side effect of merely running.
    `out_rel` is TRACKED SOURCE, so every invocation dirtied the working tree --
    and on Windows the text-mode write also flipped the file's LF endings to
    CRLF, so the rewrite dirtied the file even when the CONTENT was identical.
    A `git checkout`/`git stash` cleaning up that spurious dirt is then free to
    discard unrelated edits in the same file. A checker that mutates the tree it
    is checking cannot be run safely; verifying is the correct default.

    Returns 0 on match/write, 1 on drift.
    """
    out = os.path.join(ROOT, out_rel)
    want = render_report(rows, violations)

    if write:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        # newline="\n": never emit CRLF, so a regen on Windows does not dirty
        # the file through line endings alone.
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(want)
        print("  report REGENERATED -> %s" % out_rel)
        return 0

    try:
        with open(out, "r", encoding="utf-8", newline="") as f:
            have = f.read()
    except FileNotFoundError:
        print("FAIL: report is missing: %s\n"
              "      regenerate with: py -3 scripts/check-gated-layout-macros.py "
              "--write" % out_rel, file=sys.stderr)
        return 1

    # Compare on normalized endings: a pure CRLF/LF difference is not drift
    # worth failing a contract run over, and is exactly what the old
    # unconditional rewrite kept manufacturing.
    if have.replace("\r\n", "\n") == want:
        return 0

    print("FAIL: %s is STALE -- it does not match what the current tree "
          "generates.\n"
          "      This checker no longer rewrites it automatically (doing so "
          "silently dirtied\n"
          "      tracked source and destroyed unrelated in-flight edits).\n"
          "      Regenerate deliberately and review the diff:\n"
          "        py -3 scripts/check-gated-layout-macros.py --write"
          % out_rel, file=sys.stderr)
    return 1


def main():
    quiet = "--quiet" in sys.argv or "-q" in sys.argv
    negative = "--negative-test" in sys.argv
    # Opt-in regeneration. Default is verify-only: this script must never
    # mutate tracked source as a side effect of being run.
    write = "--write" in sys.argv

    cmake_text = read_cmake_text()
    try:
        macro_defs = parse_macro_target_defs(cmake_text)
    except ValueError as exc:
        print("GATED-LAYOUT-MACRO-GUARD: FAIL: %s" % exc, file=sys.stderr)
        return 1

    rows, violations = analyze(macro_defs)
    out_rel = "docs/render-backend-seams/gated-layout-macros.md"
    report_rc = verify_or_write_report(rows, violations, out_rel, write=write)

    if not quiet:
        print("GATED-LAYOUT-MACRO-GUARD-1 check:")
        for r in rows:
            print("  %-6s %-22s %s:%d  class %s (%s)"
                  % (r["verdict"], r["macro"], r["header"], r["line"],
                     r["class"], r["kind"]))
            if r["missing"]:
                print("         consuming=%s  MISSING=%s"
                      % (r["consuming"], r["missing"]))
        print("  report %s %s"
              % ("REGENERATED ->" if write else "verified (read-only)", out_rel))

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

    # A stale report is a real failure: the tracked doc no longer describes the
    # tree. Reported after violations so a genuine ODR violation still wins.
    if report_rc:
        return report_rc

    if not quiet:
        acks = [r for r in rows if r["verdict"] == "ACK"]
        print("GATED-LAYOUT-MACRO-GUARD: PASS (%d guard(s); %d clean, %d "
              "acknowledged-residual via allowlist)"
              % (len(rows), len(rows) - len(acks), len(acks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

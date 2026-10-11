#!/usr/bin/env python3
"""check-raw-gl-blendfunc.py -- RAW-GL-BLENDFUNC-DIFF-GATE-1 (Phase 8 enforcement).

North-star Phase 8 FOURTH (and last-for-now) enforcement gate. Bans NEW raw
blend-FUNC / blend-EQUATION call sites on ADDED diff lines, outside a
sanctioned-file allowlist. Regression-proof: it freezes the current backlog and
blocks new bypasses of the sanctioned blend-state chokepoint (applyPipeline /
GlScopedBlend).

Exact clone of scripts/check-raw-gl-colormask.py, swapped to the blend-func axis.
Catches the whole family via the regex \\bglBlend(Func|FuncSeparate|Equation|
EquationSeparate)i?\\s*\\( :
    glBlendFunc, glBlendFunci,
    glBlendFuncSeparate, glBlendFuncSeparatei,
    glBlendEquation, glBlendEquationSeparate (+ i).

It does NOT gate glEnable(GL_BLEND) / glDisable(GL_BLEND): blend on/off is
globally managed (per the ambient-ledger lesson). Only the blend FUNC/EQUATION
(the meaningful contracted state) is the gated axis.

This is DISTINCT from the descriptive PipelineDesc state-table checks: this gate
bans the raw CALL SITE itself, pushing blend FUNC/EQUATION state through the
sanctioned emitter rather than scattered glBlend* calls. Non-overlapping
mechanism.

Modeled on scripts/check-raw-gl-colormask.py Mode A (diff vs base ref, inspect
ADDED lines only, allowlist, exit 1 on unsanctioned new occurrence).

Two modes:
  * Mode A (default, ENFORCEMENT): inspect only the lines ADDED in a diff vs a
    base ref. Any newly-added raw blend-func/equation whose file is NOT in the
    allowlist fails the check (exit 1). Wire this into pre-commit/CI so the
    backlog cannot grow.
  * Mode B (--all, AUDIT): walk the render TUs and list EVERY existing raw
    blend-func/equation site with file:line + variant + per-file counts.
    Advisory, exit 0 -- documents the frozen v1 backlog.

Comment-only matches (// ... or /* ... */) are ignored.

Pure stdlib + git subprocess.

Exit code: Mode A -> 1 if any new unsanctioned raw blend-func/equation, else 0.
           Mode B -> always 0.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# glBlendFunc / glBlendFuncSeparate / glBlendEquation / glBlendEquationSeparate,
# each optionally indexed (...i). Allow whitespace after the name, before the paren.
BLENDFUNC_RE = re.compile(
    r"\bglBlend(Func|FuncSeparate|Equation|EquationSeparate)i?\s*\(")

SRC_EXT = ("*.cpp", "*.h", "*.hpp", "*.cc", "*.cxx")

# ---------------------------------------------------------------------------
# ALLOWLIST -- sanctioned files that MAY contain raw blend-func/equation calls.
# Only NEW raw calls in NON-allowlisted files trip Mode A.
#
# v1 = file-level freeze: a sanctioned/allowlisted file may add raw calls; new
#      files may not. This freezes the backlog so it cannot grow in new TUs.
# v2 tightening (future) = per-file count ceiling: record the baseline raw-call
#      count per backlog file (from Mode B), then fail if a backlog file ADDS
#      a raw call beyond its baseline -- so even backlog files can't grow. Keep
#      v1 simple (file-level) for this enforcement slice.
# ---------------------------------------------------------------------------

# Exact-path sanctioned sites (the chokepoint, the RAII wrapper, debug, editor).
ALLOWLIST_EXACT = {
    # applyPipeline emitter -- the sanctioned chokepoint for blend state
    # (emits glBlendFunc from PipelineDesc).
    "GameOS/gameos/pipeline_binder.cpp",
    # RAII / scoped GL-state wrapper (blend scope, if/when present).
    "GameOS/gameos/gl_state_guard.h",
    # debug-only renderer.
    "GameOS/gameos/debug_renderer.cpp",
    # editor TU, out of the frame loop.
    "editor/EditorGameOS.cpp",
}

# Prefix-matched sanctioned trees (out-of-engine tools, offline harnesses).
ALLOWLIST_PREFIX = (
    "tools/asset_viewer/",
    # GlStateGuard harness: offline test of the guards themselves -- must issue raw GL.
    "tools/glstate_guard_harness/",
    "tests/",
)

# FROZEN BACKLOG (v1): existing raw blend-func/equation sites, allowlisted by
# EXACT file so they do not retro-fail. Documented as the v1 freeze. New raw
# calls in NEW (non-allowlisted) files still fail. v2 will add per-file count
# ceilings.
FROZEN_BACKLOG = {
    "GameOS/gameos/gameos_graphics_bridge.cpp",
    "GameOS/gameos/gameos_graphics_capi.cpp",
    "GameOS/gameos/gameos_graphics_draw.cpp",
    "GameOS/gameos/gameos_graphics_hud.cpp",
    "GameOS/gameos/gameos_graphics_water.cpp",
    "GameOS/gameos/gos_postprocess.cpp",
    "GameOS/gameos/gos_postprocess_effects.cpp",
    "GameOS/gameos/gos_particle_bridge.cpp",
    "GameOS/gameos/gos_particle_bridge_tube.cpp",
    "GameOS/gameos/gos_vfx_mesh_bridge.cpp",
}

ALLOWLIST_EXACT_ALL = ALLOWLIST_EXACT | FROZEN_BACKLOG


def is_allowlisted(path):
    """path is a repo-relative posix path (forward slashes)."""
    if path in ALLOWLIST_EXACT_ALL:
        return True
    for pref in ALLOWLIST_PREFIX:
        if path.startswith(pref):
            return True
    return False


def strip_comments(line):
    """Remove // line comments and /* */ block comments (single-line heuristic).

    Mirrors check-raw-gl-colormask.py's intent of not counting comment-only
    matches. Block comments spanning multiple lines are handled conservatively
    by the callers (Mode B tracks block-comment state across lines)."""
    # drop // ... to end of line
    line = re.sub(r"//.*$", "", line)
    # drop inline /* ... */
    line = re.sub(r"/\*.*?\*/", "", line)
    return line


def git(args):
    return subprocess.run(["git"] + args, cwd=REPO_ROOT,
                          capture_output=True, text=True, errors="replace", **console_child.child_kwargs(redirected=True))


def default_base():
    """git merge-base HEAD origin/main, else HEAD."""
    r = git(["merge-base", "HEAD", "origin/main"])
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip()
    return "HEAD"


def mode_a(base, quiet=False):
    """Scan ADDED diff lines for new raw blend-func/equation in non-allowlisted files."""
    r = git(["diff", base, "--"] + list(SRC_EXT))
    if r.returncode != 0:
        print("[check-raw-gl-blendfunc] ERROR: git diff failed: %s"
              % r.stderr.strip(), file=sys.stderr)
        return 1
    cur_file = "?"
    findings = []  # (file, added_text)
    for line in r.stdout.splitlines():
        if line.startswith("+++ b/"):
            cur_file = line[6:]
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if not line.startswith("+"):
            continue
        added = line[1:]
        if not BLENDFUNC_RE.search(strip_comments(added)):
            continue
        if is_allowlisted(cur_file):
            continue
        findings.append((cur_file, added.strip()))

    # de-dup while preserving order
    seen = set()
    uniq = []
    for f in findings:
        if f not in seen:
            seen.add(f)
            uniq.append(f)

    if not quiet:
        print("[check-raw-gl-blendfunc] Mode A (enforcement) -- base %s" % base)
    if not uniq:
        if not quiet:
            print("[check-raw-gl-blendfunc] OK: no newly-added raw "
                  "blend-func/equation outside sanctioned sites")
        return 0
    for fpath, text in uniq:
        print("RAW-GL-BLENDFUNC-NEW %s    %s" % (fpath, text))
    print("[check-raw-gl-blendfunc] FAIL: %d new raw blend-func/equation call(s) "
          "outside the sanctioned allowlist (route blend state through "
          "applyPipeline / the scoped blend wrapper)" % len(uniq), file=sys.stderr)
    return 1


def walk_source():
    for root, dirs, files in os.walk(REPO_ROOT):
        dirs[:] = [d for d in dirs
                   if d not in (".git", "build64", "3rdparty", ".claude")]
        for fn in files:
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc", ".cxx")):
                continue
            yield os.path.join(root, fn)


def variant_of(text):
    """Classify which blend-func/equation variant a match string is."""
    # order matters: check the longer names first
    for name in ("glBlendFuncSeparate", "glBlendEquationSeparate",
                 "glBlendFunc", "glBlendEquation"):
        if text.startswith(name):
            # note an indexed suffix if present
            rest = text[len(name):]
            return name + ("i" if rest[:1] == "i" else "")
    return "glBlend?"


def mode_b():
    """List every existing raw blend-func/equation site (advisory)."""
    per_file = {}  # rel_path -> [(line_no, variant), ...]
    for path in walk_source():
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                raw_text = f.read()
        except OSError:
            continue
        hits = []
        # STRIPPER-AUDIT-1 (2026-07-31): comment-strip the WHOLE FILE with the
        # shared lexer instead of the hand-rolled per-line block tracker that
        # used to live here. That tracker computed its opener index on the RAW
        # line (`line.rfind("/*")`) AFTER stripping `//` into `scan`, so a stock
        # MechCommander banner --
        #     //****************************************************************
        # -- armed in_block (`//` followed by `*` IS a `/*` at offset 1) and the
        # scanner then skipped every following line until some unrelated `*/`.
        # MEASURED across GameOS/ code/ mclib/ editor/: 264 files affected,
        # 67,983 lines of real code skipped, hiding 10 genuine raw blend-func/equation call
        # sites from this backlog listing -- including gameos_graphics.cpp:3220 glBlendFunc(savedSrcRGB, savedDstRGB).
        # Mode B is advisory (always exit 0) and no gate consumes its numbers
        # today, so no verdict was wrong; what was wrong was the AUDIT, and this
        # file's own footer promises "v2 will add per-file count ceilings".
        # Freezing a ratchet onto an undercount is how a guard is born blind.
        # The lexer preserves line structure exactly, so line numbers are
        # unchanged. Pre-filter on the RAW text first -- EXACT, not an
        # approximation, since stripping only ever DELETES characters.
        if not BLENDFUNC_RE.search(raw_text):
            continue
        for i, scan in enumerate(
                checklib.strip_comments(raw_text).splitlines(), 1):
            for m in BLENDFUNC_RE.finditer(scan):
                hits.append((i, variant_of(scan[m.start():])))
        if hits:
            per_file[os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")] = hits

    total = sum(len(v) for v in per_file.values())
    print("[check-raw-gl-blendfunc] Mode B (audit) -- frozen raw blend-func/equation backlog")
    print("[check-raw-gl-blendfunc] total raw blend-func/equation sites: %d across %d file(s)"
          % (total, len(per_file)))
    print("")
    for fpath in sorted(per_file):
        hits = per_file[fpath]
        counts = {}
        for _, v in hits:
            counts[v] = counts.get(v, 0) + 1
        sanctioned = is_allowlisted(fpath)
        tag = "ALLOWLISTED" if sanctioned else "UNSANCTIONED(!)"
        summary = " ".join("%s=%d" % (k, counts[k]) for k in sorted(counts))
        print("  %-52s %2d  %s  [%s]" % (fpath, len(hits), summary, tag))
        for ln, v in hits:
            print("       %s:%d  %s" % (fpath, ln, v))
    print("")
    print("[check-raw-gl-blendfunc] (advisory; exit 0) v1 freezes these files; "
          "v2 will add per-file count ceilings.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=None,
                    help="base ref for diff (default: merge-base HEAD origin/main)")
    ap.add_argument("--all", action="store_true",
                    help="Mode B: list the full raw blend-func/equation backlog (advisory)")
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter (Mode A); offenders still printed")
    args = ap.parse_args()

    if args.all:
        return mode_b()

    base = args.base or default_base()
    return mode_a(base, quiet=args.quiet)


if __name__ == "__main__":
    sys.exit(main())

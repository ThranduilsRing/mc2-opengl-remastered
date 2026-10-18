#!/usr/bin/env python3
"""check-res-aspect-contract.py — META-FIX design D (resolution/aspect contract).

Static enforcement of the two-space resolution invariant (issues #45 / #49 /
HUD-RES-CLAMP merge-drop class). The contract (docs/critical_inline_rules.md):

  * Environment.screenWidth/Height is the LOGICAL 800x600 legacy-HUD canvas
    (clamped while g_hudResClampEnabled, default true).
  * Environment.drawableWidth/Height is the PHYSICAL GL drawable.
  * Modern render code NEVER reads Environment.screenWidth/Height — it uses
    Environment.drawableWidth/Height, gos_GetEnvironmentDrawableSize() or
    gos_ComputeHudCanvasBox().

RULE A (zero-tolerance): the token Environment.screenWidth/Height must not
  appear in RenderCore/, RenderWorld/, GameAdapters/, GuiRuntime/,
  EditorBridge/, HostServices/, shaders/ (all seven verified 0 at landing).
  Note: NOT redundant with check-include-firewall.sh — its SCOPE_DIRS does not
  cover GameAdapters/, the bridge layer likeliest to launder legacy reads.

RULE B (count ratchet): per-file counts of BOTH tokens (comment-stripped)
  under GameOS/gameos, mclib, Viewer against a checked-in generated baseline
  (scripts/res-aspect-baseline.json — regenerate ONLY via --update-baseline,
  never hand-edit). New reads fail; decreases print a shrink notice.
  code/, editor/, gui/ are exempt by design (authored 800-space legacy UI).

RULE C (anti-revert anchors, symbol+shape — never frozen line numbers):
  (i)   gameosmain.cpp keeps the boot clamp `Environment.screenWidth  = 800`
        + `Environment.screenHeight = 600` (HUD-RES-CLAMP).
  (ii)  gos_SetScreenMode keeps the `if (g_hudResClampEnabled)` clamp block
        (Width=800/Height=600) — protects the handleEvents rewrite
        `Environment.screenWidth = width_` on every mode change.
  (iii) gosRenderer::handleEvents keeps the #49 keep-window guard
        `reqPhysWidth > 0 && reqPhysHeight > 0` BEFORE resize_window
        (WINDOWED-8006-1).
  (iv)  mclib/camera.cpp keeps the #45 drawable-aspect override
        `Environment.drawableHeight / (float)Environment.drawableWidth`.

Runtime sibling: MC2_RES_ASSERT (RenderCore/ResolutionContract.h).
Smoke sibling: tests/smoke/matrices/resolution.json.

Usage:
  py -3 scripts/check-res-aspect-contract.py [--quiet]
  py -3 scripts/check-res-aspect-contract.py --self-test
  py -3 scripts/check-res-aspect-contract.py --update-baseline

Exit: 0 pass, 1 violation, 2 harness error (house convention).
"""

from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402

ROOT = checklib.ROOT
EXIT_OK = checklib.EXIT_OK
EXIT_VIOLATION = checklib.EXIT_VIOLATION
EXIT_HARNESS_ERROR = checklib.EXIT_HARNESS_ERROR

BASELINE_REL = "scripts/res-aspect-baseline.json"

# The seven zero-tolerance modern scopes (design D contract (c), re-verified
# zero at landing).
RULE_A_DIRS = ("RenderCore", "RenderWorld", "GameAdapters", "GuiRuntime",
               "EditorBridge", "HostServices", "shaders")
# Ratchet scopes (legacy-adjacent engine layers where reads exist today and
# must only shrink). code/, editor/, gui/ exempt: authored 800-space UI.
RULE_B_DIRS = ("GameOS/gameos", "mclib", "Viewer")

CODE_EXTS = (".cpp", ".h", ".hpp", ".inl")
SHADER_EXTS = (".vert", ".frag", ".tese", ".tesc", ".comp", ".hglsl", ".glsl")

TOKEN_RE = re.compile(r"Environment\s*\.\s*screen(Width|Height)")

SANCTIONED = ("Environment.drawableWidth/Height, "
              "gos_GetEnvironmentDrawableSize() "
              "(GameOS/gameos/gameos_graphics_params.cpp), or "
              "gos_ComputeHudCanvasBox() (gameos.hpp)")


# ---------------------------------------------------------------------------
# Context: disk + optional in-memory overlay (house pattern from
# check-split-path-contracts.py — --self-test proves failure without disk
# mutation)
# ---------------------------------------------------------------------------

class Ctx:
    def __init__(self, vfs: dict | None = None):
        self.vfs = vfs or {}

    def read(self, rel: str) -> str:
        if rel in self.vfs:
            return self.vfs[rel]
        return checklib.read_text(rel)

    def list_files(self, rel_dirs, exts):
        seen = set()
        for rel in checklib.iter_source_files(rel_dirs, exts):
            seen.add(rel)
        for rel in self.vfs:
            if rel.endswith(tuple(exts)):
                for d in rel_dirs:
                    if rel.startswith(d.rstrip("/") + "/"):
                        seen.add(rel)
                        break
        return sorted(seen)


# ---------------------------------------------------------------------------
# RULE A — zero tolerance in modern dirs
# ---------------------------------------------------------------------------

def rule_a(ctx: Ctx) -> list[str]:
    problems = []
    files = (ctx.list_files([d for d in RULE_A_DIRS if d != "shaders"], CODE_EXTS)
             + ctx.list_files(["shaders"], SHADER_EXTS))
    for rel in files:
        raw = ctx.read(rel)
        # Pre-filter on the RAW text before paying for the comment lexer.
        # EXACT, not an approximation: stripping only ever DELETES characters,
        # so a token absent from the raw text cannot appear after stripping.
        # (checklib.strip_comments is a real per-character
        # lexer rather than two regexes; without this the whole-tree walks in
        # RULE A / RULE B took res_aspect_selftest 1.28s -> 12.24s, past the
        # 5s-per-check budget this suite promises.)
        if not TOKEN_RE.search(raw):
            continue
        text = checklib.strip_comments(raw)
        for lineno, line in enumerate(text.splitlines(), start=1):
            if TOKEN_RE.search(line):
                problems.append(
                    "RULE A: %s:%d reads Environment.screenWidth/Height — "
                    "modern code must use %s" % (rel, lineno, SANCTIONED))
    return problems


# ---------------------------------------------------------------------------
# RULE B — per-file count ratchet (generated baseline, both tokens)
# ---------------------------------------------------------------------------

def current_counts(ctx: Ctx) -> dict[str, int]:
    counts: dict[str, int] = {}
    for rel in ctx.list_files(RULE_B_DIRS, CODE_EXTS):
        raw = ctx.read(rel)
        if not TOKEN_RE.search(raw):   # exact pre-filter, see rule_a()
            continue
        text = checklib.strip_comments(raw)
        n = len(TOKEN_RE.findall(text))
        if n > 0:
            counts[rel] = n
    return counts


def rule_b(ctx: Ctx) -> list[str]:
    ratchet = checklib.BaselineRatchet(BASELINE_REL)
    if not ratchet.load():
        return ["RULE B: baseline %s missing/unreadable — generate with "
                "--update-baseline" % BASELINE_REL]
    regressions, improvements = ratchet.compare(current_counts(ctx))
    problems = []
    for key, base, cur in regressions:
        problems.append(
            "RULE B: %s has %d Environment.screenWidth/Height read(s), "
            "baseline %d — new legacy-canvas reads are ratcheted; use %s "
            "(or consciously re-baseline via --update-baseline with review)"
            % (key, cur, base, SANCTIONED))
    for key, base, cur in improvements:
        print("  [ratchet-shrink notice] %s: %d -> %d (run --update-baseline "
              "to bank the improvement)" % (key, base, cur))
    return problems


# ---------------------------------------------------------------------------
# RULE C — anti-revert anchor pins (symbol + shape)
# ---------------------------------------------------------------------------

GAMEOSMAIN = "GameOS/gameos/gameosmain.cpp"
GRAPHICS_MODE = "GameOS/gameos/gameos_graphics_capi.cpp"      # gos_SetScreenMode
GRAPHICS_EVENTS = "GameOS/gameos/gameos_graphics_draw.cpp"    # gosRenderer::handleEvents
CAMERA = "mclib/camera_matrix.cpp"   # Camera::setOrthogonal (the drawable-aspect override) lives in the matrix piece


def rule_c(ctx: Ctx) -> list[str]:
    problems = []

    # (i) boot clamp restore in gameosmain.cpp
    main_text = checklib.strip_comments(ctx.read(GAMEOSMAIN))
    main_norm = checklib.normalize_ws(main_text)
    if not re.search(r"Environment\.screenWidth\s*=\s*800\s*;", main_norm):
        problems.append("RULE C(i): boot clamp `Environment.screenWidth  = 800` "
                        "missing from %s (HUD-RES-CLAMP revert?)" % GAMEOSMAIN)
    if not re.search(r"Environment\.screenHeight\s*=\s*600\s*;", main_norm):
        problems.append("RULE C(i): boot clamp `Environment.screenHeight = 600` "
                        "missing from %s (HUD-RES-CLAMP revert?)" % GAMEOSMAIN)

    gfx_text = checklib.strip_comments(ctx.read(GRAPHICS_MODE))
    ev_text = checklib.strip_comments(ctx.read(GRAPHICS_EVENTS))

    # (ii) gos_SetScreenMode clamp block — protects the handleEvents rewrite
    body = checklib.find_function_body(
        gfx_text, r"void\s+__stdcall\s+gos_SetScreenMode\s*\(")
    if body is None:
        problems.append("RULE C(ii): cannot locate gos_SetScreenMode body in %s"
                        % GRAPHICS_MODE)
    else:
        norm = checklib.normalize_ws(body)
        if not re.search(r"if\s*\(\s*g_hudResClampEnabled\s*\)", norm):
            problems.append("RULE C(ii): gos_SetScreenMode lost its "
                            "`if (g_hudResClampEnabled)` clamp branch — the "
                            "handleEvents `Environment.screenWidth = width_` "
                            "rewrite is now unclamped on every mode change")
        if not re.search(r"Width\s*=\s*800\s*;", norm) or \
           not re.search(r"Height\s*=\s*600\s*;", norm):
            problems.append("RULE C(ii): gos_SetScreenMode clamp no longer "
                            "forces the 800x600 logical canvas")

    # (iii) keep-window guard in gosRenderer::handleEvents
    body = checklib.find_function_body(
        ev_text, r"void\s+gosRenderer\s*::\s*handleEvents\s*\(\s*\)")
    if body is None:
        problems.append("RULE C(iii): cannot locate gosRenderer::handleEvents "
                        "body in %s" % GRAPHICS_EVENTS)
    else:
        norm = checklib.normalize_ws(body)
        m_guard = re.search(r"if\s*\(\s*reqPhysWidth\s*>\s*0\s*&&\s*"
                            r"reqPhysHeight\s*>\s*0\s*\)", norm)
        m_resize = re.search(r"resize_window\s*\(", norm)
        if not m_guard or not m_resize or m_guard.start() > m_resize.start():
            problems.append("RULE C(iii): #49 keep-window guard "
                            "`reqPhysWidth > 0 && reqPhysHeight > 0` no longer "
                            "precedes resize_window in handleEvents "
                            "(WINDOWED-8006-1 revert — clamped path would "
                            "force the OS window back to 800x600)")

    # (iv) drawable-aspect override in Camera::setOrthogonal
    cam_norm = checklib.normalize_ws(checklib.strip_comments(ctx.read(CAMERA)))
    if not re.search(r"Environment\.drawableHeight\s*/\s*\(\s*float\s*\)\s*"
                     r"Environment\.drawableWidth", cam_norm):
        problems.append("RULE C(iv): #45 drawable-aspect override "
                        "`Environment.drawableHeight / (float)Environment."
                        "drawableWidth` missing from %s — the perspective "
                        "frustum would revert to the clamped 4:3 canvas aspect"
                        % CAMERA)

    return problems


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def run_all(ctx: Ctx, quiet: bool) -> int:
    all_problems = []
    for label, fn in (("RULE A modern-dirs", rule_a),
                      ("RULE B ratchet", rule_b),
                      ("RULE C anchors", rule_c)):
        try:
            probs = fn(ctx)
        except OSError as e:
            print("res-aspect-contract: HARNESS ERROR in %s: %s" % (label, e),
                  file=sys.stderr)
            return EXIT_HARNESS_ERROR
        if not quiet:
            print("  %-20s %s" % (label, "OK" if not probs
                                  else "%d problem(s)" % len(probs)))
        all_problems.extend(probs)
    if all_problems:
        print("res-aspect-contract: FAIL", file=sys.stderr)
        for p in all_problems:
            print("  " + p, file=sys.stderr)
        return EXIT_VIOLATION
    if not quiet:
        print("res-aspect-contract: PASS")
    return EXIT_OK


def update_baseline() -> int:
    ratchet = checklib.BaselineRatchet(BASELINE_REL)
    counts = current_counts(Ctx())
    ratchet.save(counts)
    print("res-aspect-contract: baseline written to %s (%d file(s), %d "
          "token(s) total)" % (BASELINE_REL, len(counts), sum(counts.values())))
    return EXIT_OK


def self_test() -> int:
    """Green on tree, red on synthetic violations (house pattern
    check-contracts.sh vulkan_bindings_selftest)."""
    if run_all(Ctx(), quiet=True) != EXIT_OK:
        print("res-aspect-contract --self-test: FAIL (not green on current "
              "tree)", file=sys.stderr)
        return EXIT_VIOLATION

    failures = []

    # RULE A: a modern TU reads the legacy canvas.
    failures.append(("RULE A modern read", run_all(Ctx({
        "RenderCore/selftest_fake.cpp":
        "int f(){ return Environment.screenWidth; }\n"}), True)))

    # RULE B: a ratcheted file grows a new read.
    t = checklib.read_text(CAMERA) + \
        "\nstatic int selftestExtraRead(){ return Environment.screenWidth; }\n"
    failures.append(("RULE B ratchet grow", run_all(Ctx({CAMERA: t}), True)))

    # RULE C(ii): the SetScreenMode clamp branch is widened away (the exact
    # merge-drop class this rule guards).
    t = checklib.read_text(GRAPHICS_MODE).replace(
        "if (g_hudResClampEnabled)", "if (false && g_hudResClampEnabled)", 1)
    t = t.replace("Width  = 800;", "Width  = Width;", 1)
    failures.append(("RULE C(ii) clamp drop", run_all(Ctx({GRAPHICS_MODE: t}), True)))

    # RULE C(iii): keep-window guard removed.
    t = checklib.read_text(GRAPHICS_EVENTS).replace(
        "if (reqPhysWidth > 0 && reqPhysHeight > 0)", "if (true)", 1)
    failures.append(("RULE C(iii) #49 guard", run_all(Ctx({GRAPHICS_EVENTS: t}), True)))

    # RULE C(iv): drawable-aspect override reverted.
    t = checklib.read_text(CAMERA).replace(
        "(float)Environment.drawableHeight / (float)Environment.drawableWidth",
        "(float)Environment.screenHeight / (float)Environment.screenWidth")
    failures.append(("RULE C(iv) #45 revert", run_all(Ctx({CAMERA: t}), True)))

    bad = [name for name, rc in failures if rc == EXIT_OK]
    for name, rc in failures:
        print("  self-test %-24s %s" % (name,
              "detected (rc=%d)" % rc if rc != EXIT_OK else "NOT DETECTED"))
    if bad:
        print("res-aspect-contract --self-test: FAIL (undetected: %s)"
              % ", ".join(bad), file=sys.stderr)
        return EXIT_VIOLATION
    print("res-aspect-contract --self-test: PASS (green on tree, red on all "
          "%d synthetic violations)" % len(failures))
    return EXIT_OK


def main() -> int:
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    if "--update-baseline" in args:
        return update_baseline()
    quiet = "--quiet" in args or "-q" in args
    return run_all(Ctx(), quiet)


if __name__ == "__main__":
    sys.exit(main())

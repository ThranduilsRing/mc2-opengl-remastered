#!/usr/bin/env python3
"""BACKGROUND-SMOKE-1 -- no agent-initiated run may take focus, fullscreen, or the cursor.

WHY THIS EXISTS
---------------
The user runs other things on this machine while agent lanes work. An agent
launch that goes fullscreen, activates its window, or grabs the pointer takes
over his desktop. This has been reported and "fixed" more than once; each time
it came back through a path nobody remembered to convert.

The fix (scripts/smoke_lib/nofocus.py) is correct but BYPASSABLE, which is the
whole reason for this checker. Three independent ways to reintroduce the bug:

  RULE 1 -- roll your own launch. A new `subprocess.Popen([mc2_exe, ...])`
    without nofocus/STARTUPINFO comes up activated.

  RULE 2 -- strip the vars. Even a correct launch is defeated by an env
    allowlist or scrub that drops the windowing vars before they reach the
    child. This is exactly what happened to the capture path: scripts/
    mc2_env_pin.py is deny-by-default, so MC2_WINDOWED / MC2_WIDTH /
    MC2_HEIGHT / MC2_NO_CURSOR_GRAB were silently removed and the busiest
    launch path in the project still went fullscreen. STARTUPINFO alone does
    NOT save you: it governs only the first ShowWindow(SW_SHOWDEFAULT), and
    SDL calls ShowWindow(SW_SHOW) explicitly. The env var is load-bearing.

  RULE 3 -- raw focus/cursor Win32. SetForegroundWindow / SetCursorPos /
    ClipCursor / mouse_event / pyautogui warps outside the two modules whose
    job is cursor policy.

NOFOCUS-FINISH-1 adds the three rules below. They exist because
MC2_WINDOW_OFFSCREEN (NOFOCUS-HARDEN-1) fixed the window POSITION but opened a
second, quieter failure mode: a harness that screenshots the DESKTOP COMPOSITE
sees no pixels from an offscreen window, so it saves the user's WALLPAPER and
grades it. A confident wrong verdict is worse than a visible window, and the
only defence that scales is making the two populations declare themselves.

  RULE 6 -- composite-capture registry integrity. A file that both launches the
    engine and grabs the desktop composite (pyautogui / mss / PIL.ImageGrab)
    must be listed in COMPOSITE_CAPTURE_FILES with a reason; and every listed
    file must still actually grab, so the list cannot rot into fiction.

  RULE 7 -- offscreen declaration agreement. A listed file's launches must pass
    `offscreen=False` explicitly (the source then states its requirement, and
    nofocus refuses it outside a user-present session instead of silently
    producing wallpaper). An UNlisted file must never pass `offscreen=False` --
    that is how a normal harness quietly reclaims the user's desktop.

  RULE 8 -- offscreen delivery. Setting the var in nofocus is worthless if the
    launch path never delivers it. Every non-composite engine-launch file must
    either go through nofocus.spawn/run, reference OFFSCREEN_ENV, or name
    MC2_WINDOW_OFFSCREEN in real code. This is the rule that catches a
    hand-rolled subprocess.Popen with its own env dict.

RULE 4 and RULE 5 are RESERVED, not missing: `scripts/smoke_lib/isolation.py`
(DESKTOP-ISOLATION-ENFORCE-1, still uncommitted) already cites them by number
for desktop-isolation enforcement and the MC2_HARNESS_ALLOW_FOREGROUND ban. Do
not reuse those numbers -- renumbering a rule that another file names by number
is how a docstring becomes a lie.

CONVENTION (commit f99cf37d): verify-only by default, never mutates a tracked
file as a side effect; --write regenerates the inventory; newline="\\n".

Usage:
  py -3 scripts/check-background-launch.py [--quiet] [--write]
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "docs" / "background-launch-inventory.md"

SCAN_DIRS = ("scripts", "tools", ".claude")

#: The vars that keep an agent run off the user's screen. Any launch path that
#: can prevent these reaching the child engine is a regression.
#:
#: MC2_WINDOW_OFFSCREEN joined the list in NOFOCUS-FINISH-1 and is the one that
#: matters most: the other four make the window small, windowed and
#: unactivated, but it is still ON the desktop. Only this one keeps it off,
#: and it was being scrubbed -- scripts/mc2_env_pin.py is deny-by-default and
#: did not name it, so scripts/run_visual_capture.py (which builds its env
#: through env_pin, not through nofocus.apply_env) delivered an inert var to
#: the engine and fell back to the 250ms poller -- i.e. exactly the
#: multi-second desktop-covering flash NOFOCUS-HARDEN-1 was written to delete.
WINDOWING_VARS = ("MC2_WINDOWED", "MC2_WIDTH", "MC2_HEIGHT", "MC2_NO_CURSOR_GRAB",
                  "MC2_WINDOW_OFFSCREEN")

#: NOFOCUS-FINISH-1. Harnesses that capture the DESKTOP COMPOSITE rather than
#: reading the engine's own framebuffer. This is the constant the nofocus
#: apply_env() docstring names -- it was cited before it existed, which is the
#: same class of defect as a documented default that contradicts its reader.
#:
#: WHY A LIST AND NOT A HEURISTIC: the two capture families are
#: indistinguishable from the launch side and opposite in what they need.
#:   * ENGINE READBACK (glReadPixels / FBO -- MC2_VISUAL_CAPTURE,
#:     MC2_VISUAL_DIFF_CAPTURE, MC2_SCREENSHOT_SOURCE=backbuffer): reads the GL
#:     drawable, which keeps its full size wherever the window sits. Offscreen
#:     is free and correct. run_visual_capture.py, mech_shot.py,
#:     mechbay_preview_harness.py, debug_view_sweep.py, renderdoc/capture.py.
#:   * DESKTOP COMPOSITE (pyautogui / mss / PIL.ImageGrab): reads the input
#:     desktop. An offscreen window contributes NOTHING, so the grab succeeds
#:     and returns the user's wallpaper, which then gets graded as the game.
#:
#: The value is the reason, so a reader can tell a deliberate exception from an
#: unconverted straggler. Porting a harness to the engine readback path retires
#: its entry -- that is the intended end state for all of these.
COMPOSITE_CAPTURE_FILES = {
    "scripts/camera_sweep_smoke.py":
        "pyautogui.screenshot() of the whole desktop after a scripted arrow-key "
        "camera sweep; the sweep drives the engine through synthetic keys, so it "
        "needs a real on-screen window.",
    "scripts/capture_baseline.py":
        "V-BASELINE-0 golden capture via pyautogui.screenshot(). The highest-risk "
        "entry: a wallpaper frame here becomes a checked-in visual BASELINE.",
    "scripts/game_auto.py":
        "the interactive synthetic-input driver (keybd_event/mouse_event); its "
        "`screenshot` verb is pyautogui and its clicks target screen coordinates.",
    "scripts/quick_shot.py":
        "one-shot diagnostic pyautogui.screenshot() of the desktop.",
    "scripts/quick_sweep.py":
        "timed series of pyautogui.screenshot() desktop grabs.",
    "scripts/smoke_with_screenshots.py":
        "per-mission pyautogui.screenshot() desktop grabs alongside a smoke run.",
    "scripts/water_visual_diff.py":
        "A/B water fastpath comparison from two pyautogui.screenshot() grabs.",
    "tools/menu_oracle.py":
        "PIL.ImageGrab.grab(bbox=<window client rect>) -- it reads the DESKTOP "
        "pixels under the window rect, so the window must be on the desktop. "
        "Already launches show_window=True; NOFOCUS-FINISH-1 decoupled "
        "show_window from offscreen, so it now has to say offscreen=False too.",
}

#: Calls that read the desktop composite. Matched on the AST call name, so a
#: docstring or comment naming them is not a hit (RULE 3 learned that lesson).
COMPOSITE_CAPTURE_APIS = ("pyautogui.screenshot", "ImageGrab.grab", "mss.mss",
                          "pyautogui.locateOnScreen", "pyautogui.locateCenterOnScreen")

#: Carrier for MC2_WINDOW_OFFSCREEN in nofocus. A launch file that references it
#: is delivering the var even though the literal string never appears (RULE 8).
OFFSCREEN_ENV_NAMES = ("OFFSCREEN_ENV", "nofocus.OFFSCREEN_ENV")

#: Only these may touch focus/cursor Win32 directly -- cursor policy is their job.
CURSOR_POLICY_FILES = {
    "scripts/smoke_lib/nofocus.py",
    "scripts/smoke_lib/cursor_guard.py",
    # This checker names the forbidden APIs as string constants.
    "scripts/check-background-launch.py",
    # game_auto.py IS the synthetic-input driver. Its mouse_event calls are
    # already behind nofocus.allow_foreground() early-returns, so they cannot
    # fire in an agent run; forbidding them outright would delete the tool.
    "scripts/game_auto.py",
}

#: Files allowed to define an MC2_* deny-by-default policy. They must name every
#: windowing var as always-passthrough (verified by RULE 2).
#: Deny-by-default policies: they build the child env from an allowlist, so
#: EVERY windowing var must be named or it is silently stripped.
ENV_DENY_BY_DEFAULT = {"scripts/mc2_env_pin.py"}

#: Copy-then-override policies (os.environ.copy()): ambient values already flow
#: through, so only the vars the harness must FORCE need naming.
ENV_FORCE_POLICIES = {"scripts/smoke_lib/runner.py"}
#: MC2_WINDOW_OFFSCREEN is here because runner.py builds its own STARTUPINFO and
#: its own Popen -- it never goes through nofocus.apply_env, so the OFFSCREEN_ENV
#: default cannot reach it. Without an explicit force, every tier-1 smoke run
#: puts an options.cfg-sized window on the user's desktop.
FORCED_MINIMUM = ("MC2_WINDOWED", "MC2_NO_CURSOR_GRAB", "MC2_WINDOW_OFFSCREEN")

FOCUS_APIS = ("SetForegroundWindow", "BringWindowToTop", "SetActiveWindow",
              "SwitchToThisWindow")
CURSOR_APIS = ("SetCursorPos", "ClipCursor", "mouse_event")
PYAUTOGUI_WARPS = ("pyautogui.moveTo", "pyautogui.dragTo", "pyautogui.click",
                   "pydirectinput.moveTo")

#: Launch primitives. Built rather than written literally so this list does not
#: itself trip source scanners that flag the shell-exec helper by name.
LAUNCH_CALLS = ("subprocess.Popen", "subprocess.run", "subprocess.call",
                "subprocess.check_call", "subprocess.check_output",
                "os." + "system")

#: The sanctioned launchers. Counted in the inventory, always compliant.
SAFE_LAUNCH_CALLS = ("nofocus.spawn", "nofocus.run")

#: Heuristic for "this call launches the game engine".
#: `game_cmd` added by NOFOCUS-FINISH-1: tools/tracy_capture_compare.py builds
#: its argv into that name and hand-rolls both STARTUPINFO and Popen, so it was
#: invisible to this checker AND to nofocus -- a perf capture that shows a
#: genuinely visible, full-size, unparked window for the whole run.
ENGINE_ARGV = re.compile(
    r"mc2\.exe|EditRel|Mission Editor\.exe|\bGAME_EXE\b|\bDEFAULT_EXE\b|"
    r"\bexe_path\b|\bcfg\.exe\b|\bargs\.exe\b|\bproc_args\b|\bgame_exe\b|"
    r"\bgame_cmd\b",
    re.IGNORECASE)


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def iter_py():
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*.py")):
            if "__pycache__" in p.parts or "worktrees" in p.parts:
                continue
            yield p


def call_name(node: ast.Call) -> str:
    f = node.func
    parts = []
    while isinstance(f, ast.Attribute):
        parts.append(f.attr)
        f = f.value
    if isinstance(f, ast.Name):
        parts.append(f.id)
    return ".".join(reversed(parts))


def docstring_nodes(tree: ast.AST) -> set:
    """id() of every string Constant that is a module/class/function docstring.

    NOFOCUS-FINISH-1. `ast.parse` already discards comments, so any surviving
    string constant is code -- EXCEPT a docstring, which is prose that happens
    to be a node. Two rules shipped this session passed their own self-tests
    while satisfied by nothing more than the var name appearing in prose, so
    "the source mentions it somewhere" is not evidence that the source DOES it.
    """
    out = set()
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef,
                                 ast.AsyncFunctionDef)):
            continue
        body = getattr(node, "body", None)
        if not body:
            continue
        first = body[0]
        if (isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant)
                and isinstance(first.value.value, str)):
            out.add(id(first.value))
    return out


def names_in_code(tree: ast.AST, needle: str) -> bool:
    """True iff `needle` appears in EXECUTABLE source: a non-docstring string
    constant, an identifier, or an attribute name. Comments and docstrings do
    not count -- see docstring_nodes()."""
    docs = docstring_nodes(tree)
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            if id(node) not in docs and needle in node.value:
                return True
        elif isinstance(node, ast.Name) and node.id == needle:
            return True
        elif isinstance(node, ast.Attribute) and node.attr == needle:
            return True
    return False


def const_kwarg(node: ast.Call, name: str):
    """The literal value of keyword `name`, or `_MISSING` if absent/non-literal."""
    for kw in node.keywords:
        if kw.arg == name and isinstance(kw.value, ast.Constant):
            return kw.value.value
        if kw.arg == name:
            return _NON_LITERAL
    return _MISSING


class _Sentinel:
    def __init__(self, tag): self.tag = tag
    def __repr__(self): return self.tag


_MISSING = _Sentinel("<missing>")
_NON_LITERAL = _Sentinel("<non-literal>")


def check():
    problems: list[str] = []
    launches: list[tuple[str, int, str]] = []
    #: file -> [(Call node, call name)] for every detected engine launch.
    launch_nodes: dict[str, list] = {}
    #: file -> [(lineno, api)] for every desktop-composite grab.
    composite_users: dict[str, list] = {}
    #: file -> [lineno] for every `offscreen=False` literal keyword.
    offscreen_false_decls: dict[str, list] = {}
    #: files that put MC2_WINDOW_OFFSCREEN into a child env themselves.
    delivers_offscreen: set[str] = set()

    for path in iter_py():
        r = rel(path)
        try:
            src = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        try:
            tree = ast.parse(src)
        except SyntaxError:
            continue

        # ---- RULE 3: raw focus/cursor Win32 outside the policy modules ----
        # AST, not grep: a docstring or a string constant naming the API is not
        # a call, and flagging those made the checker cry wolf.
        if r not in CURSOR_POLICY_FILES:
            for node in ast.walk(tree):
                if not isinstance(node, ast.Call):
                    continue
                nm = call_name(node)
                leaf = nm.rsplit(".", 1)[-1]
                if nm.startswith("nofocus."):
                    continue
                bad = None
                if leaf in FOCUS_APIS or leaf in CURSOR_APIS:
                    bad = leaf
                elif nm in PYAUTOGUI_WARPS:
                    bad = nm
                if bad:
                    problems.append(
                        f"{r}:{node.lineno}: raw {bad} outside cursor-policy "
                        f"modules. Use nofocus.set_foreground_window() / "
                        f"nofocus.set_cursor_pos(), which are no-ops unless "
                        f"MC2_HARNESS_ALLOW_FOREGROUND=1.")

        # ---- RULE 1: engine launches must be non-activating ----
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            name = call_name(node)
            if name not in LAUNCH_CALLS and name not in SAFE_LAUNCH_CALLS:
                continue
            seg = ast.get_source_segment(src, node) or ""
            # A nofocus.* call is BY DEFINITION an engine launch -- that module
            # exists for nothing else -- so it needs no argv heuristic.
            if name not in SAFE_LAUNCH_CALLS and not ENGINE_ARGV.search(seg):
                continue
            # These name mc2.exe but do not LAUNCH the engine:
            #   * process queries (taskkill/tasklist/wmic/pgrep/Get-Process)
            #   * running a PYTHON script that merely takes an exe path arg
            #     (sys.executable ...) -- e.g. check-deploy-coherence.py
            if re.search(r"taskkill|tasklist|wmic|pgrep|Get-Process|powershell"
                         r"|sys\.executable", seg, re.IGNORECASE):
                continue
            launches.append((r, node.lineno, name))
            launch_nodes.setdefault(r, []).append((node, name))
            if name in SAFE_LAUNCH_CALLS or r in CURSOR_POLICY_FILES:
                continue
            kwargs = {k.arg for k in node.keywords if k.arg}
            splat = any(k.arg is None for k in node.keywords)
            ok = ("startupinfo" in kwargs or (splat and "nofocus" in seg))
            if not ok:
                problems.append(
                    f"{r}:{node.lineno}: {name}() launches the engine without "
                    f"a non-activating STARTUPINFO. Use nofocus.spawn()/"
                    f"nofocus.run(), or pass **nofocus.popen_kwargs().")

        # ---- RULE 2: env policies must not strip the windowing vars ----
        if r in ENV_DENY_BY_DEFAULT or r in ENV_FORCE_POLICIES:
            required = (WINDOWING_VARS if r in ENV_DENY_BY_DEFAULT
                        else FORCED_MINIMUM)
            for var in required:
                # names_in_code(), not `var in src`: a policy file that only
                # MENTIONS the var in a comment or docstring still strips it.
                if not names_in_code(tree, var):
                    problems.append(
                        f"{r}: defines an MC2_* env policy but never names "
                        f"{var} IN CODE (a comment or docstring mention does "
                        f"not deliver it). A deny-by-default allowlist/scrub "
                        f"that omits it strips the var before it reaches the "
                        f"engine, and the run goes fullscreen / grabs the "
                        f"cursor / sits on the user's desktop even though "
                        f"STARTUPINFO looks correct. Add it to the "
                        f"harness-control passthrough (see "
                        f"mc2_env_pin.HARNESS_CONTROL) or to the forced env "
                        f"(see smoke_lib/runner.FORCED_ENV).")

        # ---- collect once for RULES 6/7/8 ----
        composite_hits = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            nm = call_name(node)
            if any(nm == api or nm.endswith("." + api)
                   for api in COMPOSITE_CAPTURE_APIS):
                composite_hits.append((node.lineno, nm))
        if composite_hits:
            composite_users[r] = composite_hits
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and const_kwarg(node, "offscreen") is False:
                offscreen_false_decls.setdefault(r, []).append(node.lineno)
        if any(names_in_code(tree, n) for n in OFFSCREEN_ENV_NAMES) or \
                names_in_code(tree, "MC2_WINDOW_OFFSCREEN"):
            delivers_offscreen.add(r)

    # ---- RULE 6: composite-capture registry integrity ----
    launch_files = {r for r, _l, _n in launches}
    for r, hits in sorted(composite_users.items()):
        if r in COMPOSITE_CAPTURE_FILES or r not in launch_files:
            continue
        where = ", ".join(f"{nm}@{ln}" for ln, nm in hits[:3])
        problems.append(
            f"{r}: launches the engine AND grabs the desktop composite "
            f"({where}), but is not in check-background-launch."
            f"COMPOSITE_CAPTURE_FILES. Under MC2_WINDOW_OFFSCREEN the engine "
            f"window contributes NO pixels to that grab, so this harness saves "
            f"the user's WALLPAPER and grades it as the game. Either port it to "
            f"the engine readback path (MC2_VISUAL_CAPTURE=1 / "
            f"MC2_SCREENSHOT_SOURCE=backbuffer), or add it to "
            f"COMPOSITE_CAPTURE_FILES with a reason and pass offscreen=False.")
    for r in sorted(COMPOSITE_CAPTURE_FILES):
        if not (ROOT / r).is_file():
            problems.append(
                f"{r}: listed in COMPOSITE_CAPTURE_FILES but the file does not "
                f"exist. Remove the stale entry.")
        elif r not in composite_users:
            problems.append(
                f"{r}: listed in COMPOSITE_CAPTURE_FILES but no longer calls a "
                f"desktop-composite capture API {COMPOSITE_CAPTURE_APIS}. If it "
                f"was ported to the engine readback path, delete the entry and "
                f"its offscreen=False -- a stale exception is a standing "
                f"licence to put a window on the user's desktop.")

    # ---- RULE 7: offscreen declaration agreement ----
    for r, nodes in sorted(launch_nodes.items()):
        is_composite = r in COMPOSITE_CAPTURE_FILES
        for node, name in nodes:
            if name not in SAFE_LAUNCH_CALLS:
                continue
            val = const_kwarg(node, "offscreen")
            if is_composite and val is not False:
                problems.append(
                    f"{r}:{node.lineno}: {name}() in a COMPOSITE_CAPTURE_FILES "
                    f"harness must pass offscreen=False explicitly (got "
                    f"{val!r}). The default is offscreen=True, which parks the "
                    f"window at (-32000,-32000) -- the grab then returns "
                    f"wallpaper and the harness reports a confident wrong "
                    f"verdict. Saying it out loud also makes nofocus refuse the "
                    f"run outside a user-present session instead of guessing.")
            if not is_composite and val is False:
                problems.append(
                    f"{r}:{node.lineno}: {name}(offscreen=False) puts an "
                    f"agent-launched engine window ON the user's visible "
                    f"desktop, and this file is not in COMPOSITE_CAPTURE_FILES "
                    f"-- so nothing here needs the desktop composite. Drop the "
                    f"argument.")
    for r in sorted(COMPOSITE_CAPTURE_FILES):
        if r in launch_files and r not in offscreen_false_decls:
            problems.append(
                f"{r}: listed in COMPOSITE_CAPTURE_FILES and launches the "
                f"engine, but never declares offscreen=False anywhere.")

    # ---- RULE 8: offscreen delivery ----
    for r in sorted(launch_files):
        if r in COMPOSITE_CAPTURE_FILES or r in CURSOR_POLICY_FILES:
            continue
        if any(n in SAFE_LAUNCH_CALLS for _node, n in launch_nodes.get(r, [])):
            continue          # nofocus.spawn/run applies OFFSCREEN_ENV itself
        if r in delivers_offscreen:
            continue
        problems.append(
            f"{r}: launches the engine with a hand-rolled subprocess call and "
            f"never delivers MC2_WINDOW_OFFSCREEN (no nofocus.spawn/run, no "
            f"OFFSCREEN_ENV reference, no MC2_WINDOW_OFFSCREEN in code). The "
            f"engine then creates its window at options.cfg size in the middle "
            f"of the desktop, and the only thing between the user and a "
            f"full-screen flash is nofocus.park_window_offscreen's 250ms poll "
            f"-- which cannot fire before the window exists. Apply "
            f"nofocus.OFFSCREEN_ENV to the child env.")

    return problems, launches


def render_inventory(launches) -> str:
    lines = [
        "# Background-launch inventory (generated)",
        "",
        "Generated by `scripts/check-background-launch.py --write`. Do not edit by hand.",
        "",
        "Every call site that launches the game/editor engine. All of these must be",
        "non-activating by default -- see `scripts/smoke_lib/nofocus.py`.",
        "",
        "| file | line | call | window |",
        "| --- | --- | --- | --- |",
    ]
    composite = set(COMPOSITE_CAPTURE_FILES)
    for r, lineno, name in sorted(launches):
        kind = "desktop-composite" if r in composite else "offscreen"
        lines.append(f"| `{r}` | {lineno} | `{name}` | {kind} |")
    lines.append("")
    lines.append(f"Total engine launch sites: {len(launches)}")
    lines.append("")
    lines.append("## Desktop-composite capture exceptions (NOFOCUS-FINISH-1)")
    lines.append("")
    lines.append("These harnesses screenshot the DESKTOP COMPOSITE (`pyautogui` / `mss` /")
    lines.append("`PIL.ImageGrab`) instead of reading the engine's `glReadPixels` FBO, so an")
    lines.append("offscreen window would hand them the user's WALLPAPER. They declare")
    lines.append("`offscreen=False`, which `nofocus.apply_env` grants only in a user-present")
    lines.append("session (`MC2_HARNESS_ALLOW_FOREGROUND=1`). Porting one to the engine")
    lines.append("readback path retires its entry. Enforced by RULES 6-7.")
    lines.append("")
    lines.append("| file | reason |")
    lines.append("| --- | --- |")
    for r in sorted(COMPOSITE_CAPTURE_FILES):
        reason = " ".join(COMPOSITE_CAPTURE_FILES[r].split())
        lines.append(f"| `{r}` | {reason} |")
    lines.append("")
    lines.append(f"Total desktop-composite exceptions: {len(COMPOSITE_CAPTURE_FILES)}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--write", action="store_true",
                    help="regenerate docs/background-launch-inventory.md")
    args = ap.parse_args()

    problems, launches = check()
    content = render_inventory(launches)

    if args.write:
        INVENTORY.parent.mkdir(parents=True, exist_ok=True)
        with open(INVENTORY, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"[background-launch] wrote {rel(INVENTORY)} "
              f"({len(launches)} launch sites)")
    elif INVENTORY.exists():
        current = INVENTORY.read_text(encoding="utf-8")
        if current != content:
            problems.append(
                f"{rel(INVENTORY)}: stale. Re-run with --write.")

    if problems:
        print(f"[background-launch] FAIL ({len(problems)} problem(s)):")
        for p in problems:
            print("  " + p)
        return 1

    if not args.quiet:
        print(f"[background-launch] OK -- {len(launches)} engine launch site(s), "
              f"all non-activating; windowing vars survive every env policy.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

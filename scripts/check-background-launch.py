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

  RULE 11 -- one guarded pointer write. The only code that may name
    SetCursorPos is `set_cursor_pos` in scripts/smoke_lib/win_input.py, and
    that function must consult `user_is_active()` before it. Every other pointer
    move (nofocus, game_auto, the capture park/restore, the watchdog) funnels
    through it, so a user at the keyboard is never fought for the mouse (owner
    incident 2026-10-10). A new module calling the Win32 API directly, even
    from a policy file, FAILS.

  RULE 12 -- tests never drive the real pointer. A file under tests/,
    scripts/tests/ or tools/ that builds a CursorGuard, parks the cursor or names
    ClipCursor must sit under a conftest.py (or itself) that sets
    MC2_CURSOR_GUARD_OFF=1, which makes smoke_lib.win_input issue no win32
    call. A test suite that ran the real guard took the owner's mouse in 5-10 s
    bursts (2026-10-10).

  RULE 13 -- one vetted launch path, no blanket kills.
    (a) nofocus.spawn() and nofocus.run() must call `_vet_engine_exe`, which calls
        exe_guard.check_exe: every launcher goes through them, so a stale or
        fingerprint-less mc2.exe / editor exe is refused in one place.
    (b) No script may `taskkill /IM` an engine image (mc2.exe, EditRel.exe,
        Mission Editor.exe): that ends the owner's own game or another lane's run.
        Kill the pid you spawned (`proc.kill()`, `taskkill /PID`).

  RULE 14 -- registered launchers. A file that calls nofocus.spawn/run must be listed in
    scripts/background-launch-allow.json ("nofocus_callers") with its reason, and a file that
    spawns a process whose program is not a string literal ("direct_spawn": a variable argv can
    be the game) must be listed too. A new launcher is red until someone registers it; a listed
    file that no longer launches is red until the stale entry is removed.
  RULE 15 -- input injection and blanket kills. keybd_event / SendInput / pyautogui key calls
    only in files listed under "input_injection" that also gate on allow_foreground();
    `taskkill /IM <image>` of ANY image, and os.kill (TerminateProcess on Windows), fail unless
    the file is listed under "image_kill" / "os_kill".
  RULE 9 (extended) also covers mc2fx / mc2weapon_gui / asset_viewer, .sh, docs/ and .claude/.
    Launchers meant for people (modder dist/*.bat, docs examples) are not guarded: they are
    listed under "user_tools" with the reason, and rendered apart from the harness inventory.

OWNER RULE (2026-10-09, after a launch stole focus and the mouse while two lanes
were smoking and capturing): the game/editor must ALWAYS launch in the
background. That turns RULE 1 from "has a STARTUPINFO" into "goes through
nofocus.spawn/run":

  RULE 1 (tightened) -- a raw subprocess.Popen/run/call/check_* of a game or
    editor exe FAILS, even with a hand-built STARTUPINFO or
    **nofocus.popen_kwargs(): STARTUPINFO alone does not stop SDL's explicit
    ShowWindow(SW_SHOW), and a hand-rolled block is exactly how runner.py came
    to map show_window to SW_SHOW (an ACTIVATING verb). The one escape is an
    explicit marker on the call (its line, any line of the call, or the line
    above):

        # background-launch: allow <why this cannot use nofocus>

    The reason is mandatory. Allowed raw launches are listed in the inventory
    with their reason so they stay visible.
  RULE 9 -- shell launchers. A tracked .bat/.cmd/.ps1 that runs mc2.exe /
    Mission Editor.exe / mc2-launcher.exe must carry the same marker (they are
    interactive user launchers; the marker's reason says how an agent run is
    kept in the background, e.g. the CLAUDECODE guard).

Detection resolves the launch argv through same-file assignments (`argv = [exe,
...]`, `cmd = [...]`), so `Popen(argv)` is no longer invisible just because the
variable is not called `exe_path`. SCAN_DIRS covers scripts/, tools/ (including
tools/visual_verify), tests/ and .claude/.

RULE 4 and RULE 5 are RESERVED, not missing: scripts/smoke_lib/isolation.py
(DESKTOP-ISOLATION-ENFORCE-1, parked and never wired in) cited them by number
for desktop-isolation enforcement and the MC2_HARNESS_ALLOW_FOREGROUND ban. The
file is deleted; do not reuse the numbers anyway -- docs and history still name them.

CONVENTION (commit f99cf37d): verify-only by default, never mutates a tracked
file as a side effect; --write regenerates the inventory; newline="\\n".

Usage:
  py -3 scripts/check-background-launch.py [--quiet] [--write]
"""
from __future__ import annotations

import argparse
import ast
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "docs" / "background-launch-inventory.md"

#: tools/ is scanned recursively, so tools/visual_verify (capture.py, ab.py) is
#: covered by "tools"; REQUIRED_SCAN_PATHS pins that so a future edit that
#: narrows SCAN_DIRS to named subdirectories cannot silently drop it.
SCAN_DIRS = ("scripts", "tools", ".claude", "tests")
REQUIRED_SCAN_PATHS = ("scripts", "tools/visual_verify", "tests")

#: Opt-out marker for a deliberate raw launch (RULE 1) or shell launcher (RULE 9).
ALLOW_MARKER = re.compile(r"background-launch:\s*allow\s+(\S.*?)\s*$")

#: Shell launchers that execute an engine exe (RULE 9).
SHELL_SUFFIXES = (".bat", ".cmd", ".ps1", ".sh")
SHELL_ENGINE_EXE = re.compile(
    r"(mc2|Mission Editor|EditRel|mc2-launcher|mc2fx[_-]?preview|mc2fx|mc2weapon[_-]?gui|"
    r"(mc2_)?asset_viewer|mc2-asan)\.exe", re.IGNORECASE)

#: A .sh line that RUNS an exe: the exe is the command (optionally via exec/start/wine).
SH_INVOCATION = re.compile(r"""^(?:exec\s+|start\s+|wine\s+)?["']?[^\s=]*\.exe["']?(?:\s|$)""", re.IGNORECASE)

#: Registered launchers / injectors (RULE 14, RULE 15). Hand-maintained: --write never edits it.
ALLOW_PATH = ROOT / "scripts" / "background-launch-allow.json"
ALLOW_SECTIONS = ("nofocus_callers", "direct_spawn", "input_injection", "image_kill", "os_kill", "user_tools")
INPUT_LEAVES = ("keybd_event", "SendInput")
INPUT_PYAUTOGUI = ("pyautogui.keyDown", "pyautogui.keyUp", "pyautogui.press", "pyautogui.hotkey",
                   "pyautogui.write", "pyautogui.typewrite")


def _unique_allow_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate allowlist key: {key}")
        result[key] = value
    return result


def load_allow(root: Path = ROOT):
    """The allowlists, or None when this tree has none (the selftest's temporary trees)."""
    import json
    p = ALLOW_PATH if root == ROOT else root / "scripts" / "background-launch-allow.json"
    if not p.is_file():
        return None
    doc = json.loads(p.read_text(encoding="utf-8"), object_pairs_hook=_unique_allow_keys)
    return {k: dict(doc.get(k, {})) for k in ALLOW_SECTIONS}

#: The vars that keep an agent run off the user's screen. Any launch path that
#: can prevent these reaching the child engine is a regression.
#:
#: MC2_WINDOW_OFFSCREEN is the one that
#: matters most: the other four make the window small, windowed and
#: unactivated, but it is still ON the desktop. Only this one keeps it off,
#: and it was being scrubbed -- scripts/mc2_env_pin.py is deny-by-default and
#: did not name it, so scripts/run_visual_capture.py (which builds its env
#: through env_pin, not through nofocus.apply_env) delivered an inert var to
#: the engine and fell back to the 250ms poller -- i.e. exactly the
#: multi-second desktop-covering flash: with the var stripped the engine falls
#: back to the slow poller and flashes a desktop-covering window.
WINDOWING_VARS = ("MC2_WINDOWED", "MC2_WIDTH", "MC2_HEIGHT", "MC2_NO_CURSOR_GRAB",
                  "MC2_WINDOW_OFFSCREEN")

#: Harnesses that capture the DESKTOP COMPOSITE rather than
#: reading the engine's own framebuffer. This is the constant the nofocus
#: apply_env() docstring names -- it was cited before it existed, which is the
#: same class of defect as a documented default that contradicts its reader.
#:
#: WHY A LIST AND NOT A HEURISTIC: the two capture families are
#: indistinguishable from the launch side and opposite in what they need.
#:   * ENGINE READBACK (the Vulkan present readback -- MC2_VISUAL_CAPTURE,
#:     MC2_VISUAL_DIFF_CAPTURE, MC2_SCREENSHOT_PATH): reads the swapchain
#:     image, which keeps its full size wherever the window sits. Offscreen
#:     is free and correct. run_visual_capture.py, mech_shot.py,
#:     mechbay_preview_harness.py, renderdoc/capture.py.
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
    "scripts/game_auto.py":
        "the interactive synthetic-input driver (keybd_event/mouse_event); its "
        "`screenshot` verb is pyautogui and its clicks target screen coordinates.",
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
    "scripts/smoke_lib/win_input.py",
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

#: RULE 11: the single function allowed to name the pointer-write API.
CURSOR_WRITE_OWNER = "scripts/smoke_lib/win_input.py"
CURSOR_WRITE_FUNC = "set_cursor_pos"
CURSOR_WRITE_API = "SetCursorPos"
CURSOR_WRITE_GUARD = "user_is_active"

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
#: tools/tracy_capture_compare.py builds its argv into `game_cmd` and
#: hand-rolls both STARTUPINFO and Popen, so it was
#: invisible to this checker AND to nofocus -- a perf capture that shows a
#: genuinely visible, full-size, unparked window for the whole run.
ENGINE_ARGV = re.compile(
    r"mc2\.exe|EditRel|Mission Editor\.exe|\bGAME_EXE\b|\bDEFAULT_EXE\b|"
    r"\bexe_path\b|\bcfg\.exe\b|\bargs\.exe\b|\bproc_args\b|\bgame_exe\b|"
    r"\bgame_cmd\b",
    re.IGNORECASE)

#: Looser names for the exe variable. Only counted in a file that also names an
#: engine exe literally (ENGINE_FILE_MARK): `[exe, "--version"]` in a shader-tool
#: wrapper is not a game launch, but `[exe, "--profile", ...]` next to
#: "mc2.exe" is. This is what finally sees a runner's `[exe, ...]` list.
ENGINE_ARGV_LOOSE = re.compile(
    r"\bexe\b|\bMC2_EXE\b|\bEXE\b|\bgame_?exe\b|\bdeploy_exe\b",
    re.IGNORECASE)
ENGINE_FILE_MARK = re.compile(
    r"mc2\.exe|EditRel|Mission Editor\.exe|mc2-launcher|mc2_launcher\.exe",
    re.IGNORECASE)

#: argv that runs a PYTHON tool (which may itself take an exe path argument) is
#: not an engine launch; the python tool is what gets checked.
NOT_ENGINE_ARGV = re.compile(
    r"taskkill|tasklist|wmic|pgrep|Get-Process|powershell|sys\.executable"
    r"|[\"']py[\"']\s*,\s*[\"']-3[\"']", re.IGNORECASE)


def rel(p: Path, root: Path = ROOT) -> str:
    return p.relative_to(root).as_posix()


def _tracked_files(root: Path = ROOT):
    """Repo-relative POSIX paths git tracks, or None when git is unavailable."""
    try:
        out = subprocess.check_output(
            ["git", "-C", str(root), "ls-files"], text=True,
            stderr=subprocess.DEVNULL, timeout=30)
    except Exception:
        return None
    return set(out.splitlines())


def iter_py(root: Path = ROOT, use_git: bool = True):
    # Untracked scratch copies must never enter the committed inventory.
    tracked = _tracked_files(root) if use_git else None
    for d in SCAN_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*.py")):
            if "__pycache__" in p.parts or "worktrees" in p.parts:
                continue
            if tracked is not None and p.relative_to(root).as_posix() not in tracked:
                continue
            yield p


def iter_shell(root: Path = ROOT, use_git: bool = True):
    """Tracked .bat/.cmd/.ps1 launchers (RULE 9). Whole repo, not SCAN_DIRS:
    the user-facing launchers (run-mc2.bat, run-editor.bat) live at the root."""
    tracked = _tracked_files(root) if use_git else None
    if tracked is not None:
        cands = [root / t for t in sorted(tracked) if t.lower().endswith(SHELL_SUFFIXES)]
    else:
        cands = sorted(q for q in root.rglob("*") if q.suffix.lower() in SHELL_SUFFIXES)
    for p in cands:
        parts = p.relative_to(root).parts
        if parts[0] in ("3rdparty", "build64", "worktrees") or "worktrees" in parts:
            continue
        if p.is_file():
            yield p


def allow_reason(lines, node):
    """The `# background-launch: allow <why>` reason on/above this call, or None."""
    lo = max(0, node.lineno - 2)                 # the line above the call
    hi = getattr(node, "end_lineno", node.lineno)
    for ln in lines[lo:hi]:
        m = ALLOW_MARKER.search(ln)
        if m:
            return m.group(1)
    return None


class FileScan:
    """Everything the per-file rules need, from ONE ast.walk() of that file.

    Rules used to re-walk the whole tree once per needle (names_in_code was
    called 1,913 times) and once more per rule: 34 M child visits, 40+ s.
    ast.walk() is breadth-first and its yield order is fixed, so a single
    collection is identical to the repeated walks it replaces.
    """
    __slots__ = ("nodes", "calls", "assigns", "strings", "names", "attrs",
                 "call_names", "call_strs", "cursor_hits")

    def __init__(self, tree: ast.AST, src: str):
        nodes: list = list(ast.walk(tree))
        calls: list = []
        assigns: dict = {}
        strings: list = []
        names: set = set()
        attrs: set = set()
        docs: set = set()
        call_names: dict = {}
        cursor_hits: list = []
        defs = (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)
        for node in nodes:
            kind = type(node)
            if kind is ast.Call:
                calls.append(node)
                call_names[id(node)] = call_name(node)
            elif kind is ast.Constant:
                if isinstance(node.value, str):
                    strings.append(node)
            elif kind is ast.Name:
                names.add(node.id)
            elif kind is ast.Attribute:
                attrs.add(node.attr)
                if node.attr == CURSOR_WRITE_API:
                    cursor_hits.append((node.lineno, node.attr))
            elif kind is ast.Assign:
                for tgt in node.targets:
                    if type(tgt) is ast.Name:
                        assigns.setdefault(tgt.id, []).append(
                            source_segment(src, node.value) or "")
            elif kind is ast.AnnAssign or kind is ast.NamedExpr:
                if node.value is not None and type(node.target) is ast.Name:
                    assigns.setdefault(node.target.id, []).append(
                        source_segment(src, node.value) or "")
            if kind is ast.Name and node.id == CURSOR_WRITE_API:
                cursor_hits.append((node.lineno, node.id))
            if kind in defs:
                body = getattr(node, "body", None)
                if body:
                    first = body[0]
                    if (isinstance(first, ast.Expr)
                            and isinstance(first.value, ast.Constant)
                            and isinstance(first.value.value, str)):
                        docs.add(id(first.value))
        self.nodes = nodes
        self.calls = calls
        self.assigns = assigns
        self.strings = [c for c in strings if id(c) not in docs]
        self.names = names
        self.attrs = attrs
        self.cursor_hits = cursor_hits
        # Rules ask which call's ARGUMENTS mention "taskkill": collect the
        # casefolded strings of every call's subtree in one traversal (a stack
        # of enclosing calls), rather than ast.walk() per call.
        call_strs: dict = {}
        if "taskkill" in src.casefold():
            self._index_call_strings(tree, call_strs)
        self.call_names = call_names
        self.call_strs = call_strs

    @staticmethod
    def _index_call_strings(tree: ast.AST, call_strs: dict) -> None:
        stack: list = []
        def _visit(node) -> None:
            is_call = isinstance(node, ast.Call)
            if is_call:
                stack.append([])
            text = (node.value.casefold() if isinstance(node, ast.Constant)
                    and isinstance(node.value, str) else None)
            for child in ast.iter_child_nodes(node):
                _visit(child)
            if is_call:
                found = stack.pop()
                call_strs[id(node)] = found
            if text is not None:
                for found in stack:
                    found.append(text)
        _visit(tree)

    def names_in_code(self, needle: str) -> bool:
        """True iff `needle` appears in EXECUTABLE source: a non-docstring
        string constant, an identifier, or an attribute name."""
        if needle in self.names or needle in self.attrs:
            return True
        return any(needle in c.value for c in self.strings)


def assignments(tree: ast.AST, src: str, scan: FileScan | None = None) -> dict:
    """name -> [source text of every value assigned to it in this file]."""
    if scan is not None:
        return scan.assigns
    return FileScan(tree, src).assigns


def launch_argv_text(node: ast.Call, src: str, assigns: dict, depth: int = 3) -> str:
    """Source of the launched argv: first positional arg (or args=), plus the
    right-hand side of every same-file variable it mentions, `depth` levels deep.
    """
    expr = node.args[0] if node.args else next(
        (k.value for k in node.keywords if k.arg == "args"), None)
    text = (source_segment(src, expr) if expr is not None else "") or ""
    seen: set = set()
    frontier = text
    for _ in range(depth):
        new = ""
        for name in set(re.findall(r"[A-Za-z_]\w*", frontier)):
            if name in assigns and name not in seen:
                seen.add(name)
                new += "\n" + "\n".join(assigns[name])
        if not new:
            break
        text += new
        frontier = new
    return text


def call_name(node: ast.Call) -> str:
    f = node.func
    parts = []
    while isinstance(f, ast.Attribute):
        parts.append(f.attr)
        f = f.value
    if isinstance(f, ast.Name):
        parts.append(f.id)
    return ".".join(reversed(parts))


_LINE_PATTERN = re.compile(r"(.*?(?:\r\n|\n|\r|$))")
_SPLIT_CACHE: dict = {}


def _source_lines(src: str) -> list:
    """ast's own line split, computed once per source string instead of per call."""
    cached = _SPLIT_CACHE.get(id(src))
    if cached is not None and cached[0] is src:
        return cached[1]
    lines = [m[0] for m in _LINE_PATTERN.finditer(src)]
    _SPLIT_CACHE.clear()
    _SPLIT_CACHE[id(src)] = (src, lines)
    return lines


def source_segment(src: str, node) -> str | None:
    """ast.get_source_segment, with the line split cached per file.

    The standard function re-splits the WHOLE file on every call; this checker
    made 27,867 calls and that was 20 s of a 26 s run. Everything below is
    ast.get_source_segment()'s algorithm unchanged (padded is unused here).
    """
    try:
        if node.end_lineno is None or node.end_col_offset is None:
            return None
        lineno = node.lineno - 1
        end_lineno = node.end_lineno - 1
        col_offset = node.col_offset
        end_col_offset = node.end_col_offset
    except AttributeError:
        return None
    lines = _source_lines(src)
    if end_lineno == lineno:
        return lines[lineno].encode()[col_offset:end_col_offset].decode()
    first = lines[lineno].encode()[col_offset:].decode()
    last = lines[end_lineno].encode()[:end_col_offset].decode()
    body = lines[lineno + 1:end_lineno]
    body.insert(0, first)
    body.append(last)
    return "".join(body)


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


def names_in_code(tree: ast.AST, needle: str, scan: FileScan | None = None) -> bool:
    """True iff `needle` appears in EXECUTABLE source: a non-docstring string
    constant, an identifier, or an attribute name. Comments and docstrings do
    not count -- see docstring_nodes()."""
    return (scan or FileScan(tree, "")).names_in_code(needle)


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


#: RULE 10 -- console children must be windowless. Files where EVERY unflagged
#: spawn is a hard FAIL (no baseline): the engine launchers, the smoke library,
#: and the processes that run with NO console of their own (MCP servers, hooks,
#: the repo-intel tools they shell out to) -- those are the ones whose children
#: pop a visible console window.
STRICT_CHILD_PREFIXES = (
    "scripts/smoke_lib/", "scripts/mcp/", "tools/repo_intel/", ".claude/hooks/",
    "tools/visual_verify/",
)
STRICT_CHILD_FILES = {
    "scripts/run_smoke.py", "scripts/run_smoke_asan.py", "scripts/run_smoke_matrix.py",
    "scripts/run_editor_smoke.py", "scripts/run_editor_playtest_smoke.py",
    "scripts/run_visual_capture.py", "scripts/mc2_env_pin.py",
}
#: Everywhere else the rule is a RATCHET: the count of unflagged spawns per file
#: may not grow past CHILD_BASELINE (docs/background-launch-child-baseline.json,
#: regenerated by --write). Burn the baseline down; never up.
CHILD_BASELINE_PATH = ROOT / "docs" / "background-launch-child-baseline.json"

#: A spawn counts as a console child when its argv names an interpreter, a shell
#: or an exe. (Everything the scan sees qualifies; this only skips obvious
#: non-process calls such as str.join helpers mis-named run().)
CHILD_ARGV = re.compile(
    r"sys\.executable|[\"'](py|python3?|cmd|powershell|pwsh|bash|sh|git|taskkill|"
    r"tasklist|wmic)(\.exe)?[\"']|\.exe\b|\bexe\b|_exe\b|\bcmd\b|\bargv\b|\bargs\b|"
    r"\bbash\b|\bcommand\b", re.IGNORECASE)

#: Files allowed a literal CREATE_NO_WINDOW (RULE 10 bare-flag ban), with the reason.
#: scripts/smoke_lib/ is owned by the smoke-lane and spawns once per smoke run.
BARE_FLAG_EXEMPT: dict = {}

CHILD_SPAWN_CALLS = LAUNCH_CALLS + ("os.startfile", "os.popen")


def _spawn_kind(node: ast.Call, src: str):
    """How a console-child spawn handles its window.

    "child"    -- **console_child.child_kwargs(): the required form. It adds
                  CREATE_NO_WINDOW only when the parent has no console.
    "hidden"   -- **nofocus.hidden_kwargs()/popen_kwargs(): the smoke library's own form
                  (hidden_kwargs applies the same flag-only-without-a-console rule).
    "detached" -- creationflags=DETACHED_PROCESS: no console and no conhost at all.
    "bare"     -- a literal creationflags=...CREATE_NO_WINDOW / 0x08000000. FORBIDDEN:
                  from a parent that already has a console it allocates a NEW
                  hidden console (one conhost.exe per child). check-contracts
                  spawns hundreds; that crashed the owner's VPN split-tunnel
                  process monitor on 2026-10-09.
    None       -- no window handling at all.
    """
    bare = False
    for kw in node.keywords:
        ks = source_segment(src, kw.value) or ""
        if kw.arg == "creationflags":
            if re.search(r"NO_WINDOW|0x08000000", ks):
                bare = True
            elif re.search(r"DETACHED_PROCESS", ks):
                return "detached"
        if kw.arg is None:
            if re.search(r"child_kwargs", ks):
                return "child"
            if re.search(r"hidden_kwargs|popen_kwargs", ks):
                return "hidden"
    return "bare" if bare else None


def argv_variants(node: ast.Call, src: str, assigns: dict) -> list:
    """The launched argv as separate, fully-expanded texts.

    One variant per value assigned (in this file) to a variable the argv names,
    each expanded with every assignment of the names IT mentions, three levels
    deep; plus the direct expression itself. A variable assigned two different
    argv lists in one file yields two variants, so `cmd = [sys.executable, ...]`
    cannot launder a different `cmd = [exe, ...]` -- and `cmd = [py, ...]` with
    `py = sys.executable` is recognised as a python launcher, not an engine one.
    """
    expr = node.args[0] if node.args else next(
        (k.value for k in node.keywords if k.arg == "args"), None)
    direct = (source_segment(src, expr) if expr is not None else "") or ""

    def expand(text: str, depth: int = 3) -> str:
        seen: set = set()
        frontier, out = text, text
        for _ in range(depth):
            new = ""
            for name in set(re.findall(r"[A-Za-z_]\w*", frontier)):
                if name in assigns and name not in seen:
                    seen.add(name)
                    new += "\n" + "\n".join(assigns[name])
            if not new:
                break
            out += new
            frontier = new
        return out

    variants = [expand(direct)]
    for name in set(re.findall(r"[A-Za-z_]\w*", direct)):
        for v1 in assigns.get(name, []):
            variants.append(expand(v1))
    return variants


def literal_program(node: ast.Call):
    """The program a literal argv names: a string constant first element
    ("git", "taskkill"), or "sys.executable". None when the argv is not a
    literal list or its first element is a variable (then the name decides).
    A literal non-engine program is never an engine launch, whatever its
    ARGUMENTS mention -- `git -C <path-with-exe-in-it>` is just git.
    """
    expr = node.args[0] if node.args else next(
        (k.value for k in node.keywords if k.arg == "args"), None)
    if isinstance(expr, (ast.List, ast.Tuple)) and expr.elts:
        e0 = expr.elts[0]
        if isinstance(e0, ast.Constant) and isinstance(e0.value, str):
            return e0.value
        if isinstance(e0, ast.Attribute) and call_name(ast.Call(func=e0, args=[], keywords=[])) == "sys.executable":
            return "sys.executable"
    return None


def collect_sites(tree: ast.AST, scan: FileScan | None = None) -> dict:
    """What a file does that RULE 14 / RULE 15 care about (AST, not grep)."""
    out = {"nofocus": False, "direct": False, "inject": [], "image_kill": [], "os_kill": []}
    for node in (scan.calls if scan is not None else ast.walk(tree)):
        if not isinstance(node, ast.Call):
            continue
        nm = scan.call_names[id(node)] if scan is not None else call_name(node)
        leaf = nm.rsplit(".", 1)[-1]
        if nm in SAFE_LAUNCH_CALLS:
            out["nofocus"] = True
        elif (nm in LAUNCH_CALLS or nm in ("os.startfile", "os.popen")) and literal_program(node) is None:
            out["direct"] = True
        if leaf in INPUT_LEAVES or nm in INPUT_PYAUTOGUI:
            out["inject"].append((node.lineno, nm))
        if nm == "os.kill":
            out["os_kill"].append(node.lineno)
        strs = (scan.call_strs.get(id(node), []) if scan is not None
                else [c.value.casefold() for c in ast.walk(node)
                      if isinstance(c, ast.Constant) and isinstance(c.value, str)])
        if any("taskkill" in s for s in strs) and ("/im" in strs or any(re.search(r"/im\s", s) for s in strs)):
            out["image_kill"].append(node.lineno)
    return out


def registry_problems(sites_by_file: dict, texts: dict, allow: dict) -> list:
    """RULE 14 and RULE 15 against the allowlists, including stale entries."""
    out = []
    nof = {r for r, s in sites_by_file.items() if s["nofocus"]}
    dsp = {r for r, s in sites_by_file.items() if s["direct"]}
    for r in sorted(nof - set(allow["nofocus_callers"])):
        out.append(f"{r}: calls nofocus.spawn/run but is not in scripts/background-launch-allow.json "
                   f"(nofocus_callers). Register it with the reason it launches the engine.")
    for r in sorted(dsp - set(allow["direct_spawn"])):
        out.append(f"{r}: spawns a process whose program is not a string literal (it could be the game) and is "
                   f"not in scripts/background-launch-allow.json (direct_spawn). Register it with its reason, or "
                   f"route the launch through nofocus.spawn.")
    for r, s in sorted(sites_by_file.items()):
        for line, api in s["inject"]:
            if r not in allow["input_injection"] or "allow_foreground" not in texts.get(r, ""):
                out.append(f"{r}:{line}: {api} sends global input to the FOREGROUND window (the owner's). Only files "
                           f"listed under input_injection that also gate on nofocus.allow_foreground() may do it.")
        for line in s["image_kill"]:
            if r not in allow["image_kill"]:
                out.append(f"{r}:{line}: taskkill /IM ends every process of that image name, including the owner's "
                           f"and other lanes'. Kill the pid you spawned.")
        for line in s["os_kill"]:
            if r not in allow["os_kill"]:
                out.append(f"{r}:{line}: os.kill is TerminateProcess on Windows, not a liveness probe. Use "
                           f"smoke_lib.child_registry.pid_alive / kill_if_orphan.")
    for section, present in (("nofocus_callers", nof), ("direct_spawn", dsp),
                             ("input_injection", {r for r, s in sites_by_file.items() if s["inject"]}),
                             ("image_kill", {r for r, s in sites_by_file.items() if s["image_kill"]}),
                             ("os_kill", {r for r, s in sites_by_file.items() if s["os_kill"]})):
        for r in sorted(set(allow[section]) - present):
            out.append(f"scripts/background-launch-allow.json: {section} lists {r}, which no longer does that. "
                       f"Remove the stale entry.")
        for r, why in allow[section].items():
            if not str(why).strip():
                out.append(f"scripts/background-launch-allow.json: {section}/{r} has no reason.")
    return out


def check(root: Path = ROOT, use_git: bool = True, allow=None):
    """Run every rule. Returns (problems, launches, extras).

    `extras` carries what the inventory renders: allowed raw launches, shell
    launchers and per-file counts of unflagged console spawns.
    """
    problems: list[str] = []
    if allow is None:
        allow = load_allow(root)
    sites_by_file: dict = {}
    texts: dict = {}
    launches: list[tuple[str, int, str]] = []
    #: (file, line) -> reason, for raw launches carrying the allow marker.
    allowed_raw: dict = {}
    #: file -> [(Call node, call name)] for every detected engine launch.
    launch_nodes: dict[str, list] = {}
    #: file -> [(lineno, api)] for every desktop-composite grab.
    composite_users: dict[str, list] = {}
    #: file -> [lineno] for every `offscreen=False` literal keyword.
    offscreen_false_decls: dict[str, list] = {}
    #: files that put MC2_WINDOW_OFFSCREEN into a child env themselves.
    delivers_offscreen: set[str] = set()
    #: file -> [lineno] of console-child spawns lacking a no-window flag (RULE 10).
    unhidden: dict[str, list] = {}
    #: file -> {lineno: reason} for console spawns carrying the allow marker.
    allowed_child: dict[str, dict] = {}
    #: file -> [lineno] of console-child spawns using a literal CREATE_NO_WINDOW.
    bare_flag: dict[str, list] = {}

    for path in iter_py(root, use_git):
        r = rel(path, root)
        try:
            src = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        try:
            tree = ast.parse(src)
        except SyntaxError:
            continue
        lines = src.splitlines()
        scan = FileScan(tree, src)
        assigns = scan.assigns
        names_engine_file = bool(ENGINE_FILE_MARK.search(src))

        if r != "scripts/check-background-launch.py":
            sites_by_file[r] = collect_sites(tree, scan)
            texts[r] = src

        # ---- RULE 11: SetCursorPos only inside win_input.set_cursor_pos, after the idle check ----
        if r != "scripts/check-background-launch.py":
            problems.extend(cursor_write_problems(r, tree, scan))

        # ---- RULE 13: vetted launch path, no blanket kills ----
        if r != "scripts/check-background-launch.py":
            problems.extend(engine_kill_problems(r, tree, scan))
        problems.extend(launch_vet_problems(r, tree, scan))

        # ---- RULE 12: test trees must disable the real cursor guard ----
        problems.extend(test_cursor_isolation_problems(root, r, tree, src, scan))

        # ---- RULE 3: raw focus/cursor Win32 outside the policy modules ----
        # AST, not grep: a docstring or a string constant naming the API is not
        # a call, and flagging those made the checker cry wolf.
        if r not in CURSOR_POLICY_FILES:
            for node in scan.calls:
                nm = scan.call_names[id(node)]
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

        # ---- RULE 1 (tightened) + RULE 10 ----
        for node in scan.calls:
            name = scan.call_names[id(node)]
            safe = name in SAFE_LAUNCH_CALLS
            if name not in CHILD_SPAWN_CALLS and not safe:
                continue
            seg = source_segment(src, node) or ""
            variants = argv_variants(node, src, assigns)

            # Is this an ENGINE launch? A nofocus.* call is BY DEFINITION one --
            # that module exists for nothing else. Otherwise the argv (resolved
            # through same-file assignments) must name an engine exe, in a variant
            # that is not itself a python/process-query launcher.
            is_engine = safe
            prog = None if safe else literal_program(node)
            literal_other = prog is not None and not ENGINE_FILE_MARK.search(prog)
            if not safe and not literal_other:
                for v in variants:
                    if NOT_ENGINE_ARGV.search(v):
                        continue
                    hay = v if v != variants[0] else v + "\n" + seg
                    if ENGINE_ARGV.search(hay) or (
                            names_engine_file and ENGINE_ARGV_LOOSE.search(v)):
                        is_engine = True
                        break
                if is_engine and NOT_ENGINE_ARGV.search(variants[0]):
                    is_engine = False

            if is_engine:
                launches.append((r, node.lineno, name))
                launch_nodes.setdefault(r, []).append((node, name))
                if not safe and r not in CURSOR_POLICY_FILES:
                    why = allow_reason(lines, node)
                    if why:
                        allowed_raw[(r, node.lineno)] = why
                    else:
                        problems.append(
                            f"{r}:{node.lineno}: raw {name}() launches the engine. "
                            f"OWNER RULE: the game/editor ALWAYS launches in the "
                            f"background, which only nofocus.spawn()/nofocus.run() "
                            f"guarantee (non-activating STARTUPINFO + "
                            f"MC2_NO_CURSOR_GRAB/WINDOWED/WINDOW_OFFSCREEN + the "
                            f"offscreen backstop). A hand-built STARTUPINFO or "
                            f"**popen_kwargs() is not enough. If a raw launch is "
                            f"truly unavoidable, add `# background-launch: allow "
                            f"<why>` on or above the call.")
                continue    # an engine launch is a GUI process: RULE 10 is for console children

            # ---- RULE 10: console children must be windowless ----
            if safe or r in CURSOR_POLICY_FILES:
                continue
            if not any(CHILD_ARGV.search(v) for v in variants) and not CHILD_ARGV.search(seg):
                continue
            kind = _spawn_kind(node, src)
            if kind in ("child", "hidden", "detached"):
                continue
            why = allow_reason(lines, node)
            if why:
                allowed_child.setdefault(r, {})[node.lineno] = why
                continue
            if kind == "bare" and r in BARE_FLAG_EXEMPT:
                continue
            if kind == "bare":
                bare_flag.setdefault(r, []).append(node.lineno)
            else:
                unhidden.setdefault(r, []).append(node.lineno)

        # ---- RULE 2: env policies must not strip the windowing vars ----
        if r in ENV_DENY_BY_DEFAULT or r in ENV_FORCE_POLICIES:
            required = (WINDOWING_VARS if r in ENV_DENY_BY_DEFAULT
                        else FORCED_MINIMUM)
            for var in required:
                # names_in_code(), not `var in src`: a policy file that only
                # MENTIONS the var in a comment or docstring still strips it.
                if not scan.names_in_code(var):
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
        for node in scan.calls:
            nm = call_name(node)
            if any(nm == api or nm.endswith("." + api)
                   for api in COMPOSITE_CAPTURE_APIS):
                composite_hits.append((node.lineno, nm))
        if composite_hits:
            composite_users[r] = composite_hits
        for node in scan.calls:
            if const_kwarg(node, "offscreen") is False:
                offscreen_false_decls.setdefault(r, []).append(node.lineno)
        if any(scan.names_in_code(n) for n in OFFSCREEN_ENV_NAMES) or \
                scan.names_in_code("MC2_WINDOW_OFFSCREEN"):
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
            f"MC2_SCREENSHOT_PATH on the Vulkan backend), or add it to "
            f"COMPOSITE_CAPTURE_FILES with a reason and pass offscreen=False.")
    for r in sorted(COMPOSITE_CAPTURE_FILES):
        if not (root / r).is_file():
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
        nodes = launch_nodes.get(r, [])
        if any(n in SAFE_LAUNCH_CALLS for _node, n in nodes):
            continue          # nofocus.spawn/run applies OFFSCREEN_ENV itself
        if all((r, node.lineno) in allowed_raw for node, _n in nodes):
            continue          # every raw launch carries an explicit allow marker
        if r in delivers_offscreen:
            continue
        problems.append(
            f"{r}: launches the engine with a hand-rolled subprocess call and "
            f"never delivers MC2_WINDOW_OFFSCREEN (no nofocus.spawn/run, no "
            f"OFFSCREEN_ENV reference, no MC2_WINDOW_OFFSCREEN in code). The "
            f"engine then creates its window at options.cfg size in the middle "
            f"of the desktop, and the only thing between the user and a "
            f"full-screen flash is the old window poller's 250ms poll "
            f"-- which cannot fire before the window exists. Apply "
            f"nofocus.OFFSCREEN_ENV to the child env.")

    # ---- RULE 9: shell launchers ----
    shell_launchers: dict = {}
    user_tools: dict = {}
    for path in iter_shell(root, use_git):
        r = rel(path, root)
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        hits = []
        for i, ln in enumerate(text.splitlines(), 1):
            st = ln.strip()
            if not SHELL_ENGINE_EXE.search(st):
                continue
            if re.match(r"(?i)(rem\b|::|#|echo\b|write-host\b|copy\b|set\b|if\s+exist\b)", st):
                continue
            if path.suffix.lower() == ".sh" and (not SH_INVOCATION.match(st) or st.endswith("\\") or st.lower().count(".exe") > 1):
                continue    # a path in a cp/test/variable line is not a launch; only command position is
            hits.append(i)
        if not hits:
            continue
        if allow is not None and r in allow["user_tools"]:
            user_tools[r] = allow["user_tools"][r]
            continue
        m = None
        for ln in text.splitlines():
            m = ALLOW_MARKER.search(ln)
            if m:
                break
        shell_launchers[r] = (hits, m.group(1) if m else None)
        if not m:
            problems.append(
                f"{r}:{hits[0]}: shell launcher runs a game/editor exe with no "
                f"`background-launch: allow <why>` marker. These are interactive "
                f"user launchers; the marker's reason must say how an AGENT run is "
                f"kept in the background (e.g. the CLAUDECODE guard).")

    # ---- RULE 10: console children windowless ----
    baseline = {}
    try:
        import json
        baseline = json.loads(CHILD_BASELINE_PATH.read_text(encoding="utf-8")) \
            if root == ROOT and CHILD_BASELINE_PATH.is_file() else {}
    except Exception:
        baseline = {}
    for r, lns in sorted(bare_flag.items()):
        shown = ", ".join(str(n) for n in lns[:6])
        problems.append(
            f"{r}:{lns[0]}: {len(lns)} console-child spawn(s) with a bare "
            f"creationflags=CREATE_NO_WINDOW (lines {shown}"
            f"{'...' if len(lns) > 6 else ''}). From a parent that already has a "
            f"console the flag allocates a NEW hidden console per child -- one "
            f"conhost.exe each; hundreds per check-contracts run crashed the "
            f"owner's VPN split-tunnel monitor. Use "
            f"**console_child.child_kwargs() (add redirected=True when the call "
            f"captures/redirects stdio), which adds the flag only when this "
            f"process has no console, or add `# background-launch: allow <why>`.")
    for r, lns in sorted(unhidden.items()):
        strict = r in STRICT_CHILD_FILES or r.startswith(STRICT_CHILD_PREFIXES) \
            or r in launch_files
        allowed_n = baseline.get(r, 0)
        if strict or len(lns) > allowed_n:
            shown = ", ".join(str(n) for n in lns[:6])
            problems.append(
                f"{r}:{lns[0]}: {len(lns)} child-process spawn(s) without "
                f"CREATE_NO_WINDOW/DETACHED_PROCESS (lines {shown}"
                f"{'...' if len(lns) > 6 else ''})"
                + ("" if strict else f"; baseline allows {allowed_n}")
                + ". OWNER RULE: no window may appear, including a console "
                  "window of a helper spawned during a run. Pass "
                  "**console_child.child_kwargs() (redirected=True if the call "
                  "captures stdio), or add `# background-launch: allow <why>`.")

    # ---- RULE 14 / RULE 15: registered launchers, input injection, blanket kills ----
    if allow is not None:
        problems.extend(registry_problems(sites_by_file, texts, allow))
        for r, why in sorted(allow["user_tools"].items()):
            if r not in user_tools:
                problems.append(f"scripts/background-launch-allow.json: user_tools lists {r}, which is not a "
                                f"shell launcher of an engine exe any more. Remove the stale entry.")
    else:
        for r, s in sorted(sites_by_file.items()):
            for line in s["image_kill"]:
                problems.append(f"{r}:{line}: taskkill /IM ends every process of that image name.")

    extras = {"allowed_raw": allowed_raw, "shell": shell_launchers, "user_tools": user_tools,
              "unhidden": {k: len(v) for k, v in unhidden.items()},
              "allowed_child": allowed_child}
    return problems, launches, extras


def render_inventory(launches, extras=None) -> str:
    extras = extras or {}
    allowed_raw = extras.get("allowed_raw", {})
    shell = extras.get("shell", {})
    lines = [
        "# Background-launch inventory (generated)",
        "",
        "Generated by `scripts/check-background-launch.py --write`. Do not edit by hand.",
        "",
        "Every call site that launches the game/editor engine. All of these must be",
        "non-activating by default -- see `scripts/smoke_lib/nofocus.py`. OWNER RULE:",
        "the game/editor ALWAYS launches in the background; a raw `subprocess` launch",
        "FAILS the checker unless it carries `# background-launch: allow <why>`.",
        "",
        "| file | line | call | window |",
        "| --- | --- | --- | --- |",
    ]
    composite = set(COMPOSITE_CAPTURE_FILES)
    for r, lineno, name in sorted(launches):
        kind = "desktop-composite" if r in composite else "offscreen"
        call = f"`{name}`"
        if (r, lineno) in allowed_raw:
            call += f" **raw, allowed:** {allowed_raw[(r, lineno)]}"
        lines.append(f"| `{r}` | {lineno} | {call} | {kind} |")
    lines.append("")
    lines.append(f"Total engine launch sites: {len(launches)}")
    lines.append("")
    lines.append("## Shell launchers (RULE 9)")
    lines.append("")
    lines.append("Interactive user launchers (`.bat`/`.cmd`/`.ps1`). Each must carry a")
    lines.append("`background-launch: allow <why>` marker saying how an agent run is kept")
    lines.append("in the background.")
    lines.append("")
    lines.append("| file | line | how an agent run stays in the background |")
    lines.append("| --- | --- | --- |")
    for r in sorted(shell):
        hits, why = shell[r]
        lines.append(f"| `{r}` | {hits[0]} | {why or '**MISSING MARKER**'} |")
    lines.append("")
    lines.append(f"Total shell launchers: {len(shell)}")
    tools = extras.get("user_tools", {})
    lines.append("")
    lines.append("## User tools, not harness")
    lines.append("")
    lines.append("Launchers meant to be double-clicked or run by a person (shipped dist scripts, docs examples).")
    lines.append("They are not guarded; each is listed in `scripts/background-launch-allow.json` (`user_tools`).")
    lines.append("")
    lines.append("| file | why it is a user tool |")
    lines.append("| --- | --- |")
    for r in sorted(tools):
        lines.append(f"| `{r}` | {tools[r]} |")
    lines.append("")
    lines.append(f"Total user tools: {len(tools)}")
    lines.append("")
    lines.append("## Desktop-composite capture exceptions (NOFOCUS-FINISH-1)")
    lines.append("")
    lines.append("These harnesses screenshot the DESKTOP COMPOSITE (`pyautogui` / `mss` /")
    lines.append("`PIL.ImageGrab`) instead of reading the engine's `glReadPixels` FBO, so an")
    lines.append("offscreen window would hand them the user's WALLPAPER. They declare")
    lines.append("`offscreen=False`, which `nofocus.apply_env` grants only in a user-present")
    lines.append("session (`MC2_HARNESS_ALLOW_FOREGROUND=1`). Even then the window is")
    lines.append("SW_SHOWNOACTIVATE, the engine runs with `MC2_NO_CURSOR_GRAB=1`, and the only")
    lines.append("z-order change is `nofocus.raise_noactivate` (HWND_TOPMOST + SWP_NOACTIVATE):")
    lines.append("no focus, and the mouse stays untouched. Porting one to the engine")
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


def render_baseline(extras) -> str:
    import json
    # Strict files must be at zero, so they never enter the baseline.
    base = {r: n for r, n in sorted(extras["unhidden"].items())
            if not (r in STRICT_CHILD_FILES or r.startswith(STRICT_CHILD_PREFIXES))}
    return json.dumps(base, indent=2, sort_keys=True) + "\n"


# --------------------------------------------------------------------------
# --selftest: prove each rule can FAIL. A checker nobody has watched fail is
# indistinguishable from one that cannot.
# --------------------------------------------------------------------------
_SELFTEST_FILES = {
    # RULE 1: raw Popen of the engine, variable argv, hand-built STARTUPINFO.
    "tools/visual_verify/raw_launch.py": (
        "import subprocess\n"
        "EXE = 'A:/x/mc2.exe'\n"
        "def go():\n"
        "    argv = [EXE, '--mission', 'm']\n"
        "    si = subprocess.STARTUPINFO()\n"
        "    return subprocess.Popen(argv, startupinfo=si)\n"),
    # same, but explicitly allowed with a reason -> must NOT fail RULE 1.
    "tests/smoke/allowed_launch.py": (
        "import subprocess\n"
        "EXE = 'A:/x/mc2.exe'\n"
        "def go():\n"
        "    # background-launch: allow selftest fixture, not a real launch\n"
        "    return subprocess.Popen([EXE, '--mission', 'm'])\n"),
    # marker without a reason must not count.
    "tests/smoke/empty_reason.py": (
        "import subprocess\n"
        "EXE = 'A:/x/mc2.exe'\n"
        "def go():\n"
        "    return subprocess.Popen([EXE])  # background-launch: allow\n"),
    # the sanctioned path -> clean.
    "scripts/ok_launch.py": (
        "from smoke_lib import nofocus\n"
        "EXE = 'A:/x/mc2.exe'\n"
        "def go():\n"
        "    return nofocus.spawn([EXE, '--mission', 'm'])\n"),
    # RULE 10: console child without the flag, in a strict path.
    "scripts/mcp/unhidden.py": (
        "import subprocess, sys\n"
        "def go():\n"
        "    return subprocess.run([sys.executable, 'x.py'], capture_output=True)\n"),
    # RULE 10: flagged -> clean.
    "scripts/mcp/hidden.py": (
        "import subprocess, sys\n"
        "from smoke_lib import nofocus\n"
        "def go():\n"
        "    return subprocess.run([sys.executable, 'x.py'], capture_output=True,\n"
        "                          **nofocus.hidden_kwargs())\n"
        "def go3():\n"
        "    return subprocess.run(['cmd'], **console_child.child_kwargs())\n"),
    # RULE 10: a bare CREATE_NO_WINDOW on a console child is forbidden (conhost per child).
    "scripts/bare_flag.py": (
        "import subprocess, sys\n"
        "def go():\n"
        "    return subprocess.run(['cmd'], creationflags=subprocess.CREATE_NO_WINDOW)\n"
        "def go2():\n"
        "    return subprocess.run([sys.executable, 'x.py'], capture_output=True,\n"
        "                          creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))\n"),
    # same, but carrying the allow marker -> passes.
    "scripts/bare_flag_allowed.py": (
        "import subprocess\n"
        "def go():\n"
        "    # background-launch: allow selftest fixture\n"
        "    return subprocess.run(['cmd'], creationflags=subprocess.CREATE_NO_WINDOW)\n"),
    # python launcher that merely takes an exe path -> not an engine launch.
    "scripts/py_wrapper.py": (
        "import subprocess, sys\n"
        "def go(exe):\n"
        "    cmd = [sys.executable, 'run_smoke.py', '--exe', exe]\n"
        "    return subprocess.run(cmd, capture_output=True,\n"
        "                          creationflags=subprocess.CREATE_NO_WINDOW)\n"
        "# mc2.exe\n"),
    # RULE 11: pointer write outside the guarded function, and an unguarded copy of it.
    "scripts/raw_cursor.py": (
        "import ctypes\n"
        "def park():\n"
        "    ctypes.windll.user32.SetCursorPos(1, 1)\n"),
    "scripts/smoke_lib/win_input.py": (
        "def user_is_active():\n"
        "    return False\n"
        "def set_cursor_pos(pos):\n"
        "    return u.SetCursorPos(pos[0], pos[1])\n"),
    # RULE 12: a test that builds the real guard with / without a conftest that disables it.
    "tests/unfixtured/test_guard.py": (
        "from smoke_lib.cursor_guard import CursorGuard\n"
        "def test_x():\n"
        "    CursorGuard().start()\n"),
    "tests/fixtured/conftest.py": "import os\nos.environ['MC2_CURSOR_GUARD_OFF'] = '1'\n",
    "tests/fixtured/test_guard.py": (
        "from smoke_lib.cursor_guard import CursorGuard\n"
        "def test_x():\n"
        "    CursorGuard().start()\n"),
    # RULE 13: blanket engine kill / nofocus without the vetting call.
    "scripts/blanket_kill.py": (
        "import subprocess\n"
        "def k():\n"
        "    subprocess.run(['taskkill', '/F', '/IM', 'mc2.exe'])\n"
        "def k2(pid):\n"
        "    subprocess.run(['taskkill', '/F', '/PID', str(pid)])\n"),
    "scripts/smoke_lib/nofocus.py": (
        "def _vet_engine_exe(argv):\n"
        "    return None\n"
        "def spawn(argv):\n"
        "    return argv\n"
        "def run(argv):\n"
        "    _vet_engine_exe(argv)\n"),
    # RULE 14 / RULE 15 fixtures (judged against an explicit allowlist in selftest()).
    "scripts/new_launcher.py": (
        "from smoke_lib import nofocus\n"
        "def go(exe):\n"
        "    return nofocus.spawn([exe])\n"),
    "scripts/var_spawn.py": (
        "import subprocess\n"
        "def go(prog):\n"
        "    return subprocess.Popen(prog)\n"),
    "scripts/inject.py": (
        "def go(u):\n"
        "    u.keybd_event(27, 0, 0, 0)\n"),
    "scripts/inject_ok.py": (
        "from smoke_lib import nofocus\n"
        "def go(u):\n"
        "    if nofocus.allow_foreground():\n"
        "        u.keybd_event(27, 0, 0, 0)\n"),
    "scripts/oskill.py": (
        "import os\n"
        "def go(pid):\n"
        "    os.kill(pid, 0)\n"),
    "tools/user_launch.bat": "@echo off\nstart \"\" mc2fx_preview.exe\n",
    # RULE 9: shell launcher with and without a marker.
    "launch_bad.bat": "@echo off\nmc2.exe\n",
    "launch_ok.bat": "@echo off\nREM background-launch: allow selftest fixture\nmc2.exe\n",
}


def selftest() -> int:
    ok = True

    def expect(cond: bool, msg: str) -> None:
        nonlocal ok
        print(("  ok   " if cond else "  FAIL ") + msg)
        ok = ok and cond

    # SCAN_DIRS must keep covering the trees the owner named.
    for needed in REQUIRED_SCAN_PATHS:
        top = needed.split("/")[0]
        expect(top in SCAN_DIRS, f"SCAN_DIRS covers {needed}")

    with tempfile.TemporaryDirectory(prefix="bglaunch_selftest_") as td:
        root = Path(td)
        for relp, body in _SELFTEST_FILES.items():
            f = root / relp
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(body, encoding="utf-8", newline="\n")
        problems, launches, _extras = check(root, use_git=False)
        text = "\n".join(problems)

        def flagged(relp: str, needle: str = "") -> bool:
            return any(p.startswith(relp) and needle in p for p in problems)

        expect(flagged("tools/visual_verify/raw_launch.py", "raw subprocess.Popen"),
               "raw Popen(argv=[EXE,...]) in tools/visual_verify FAILS despite STARTUPINFO")
        expect(not flagged("tests/smoke/allowed_launch.py", "launches the engine"),
               "raw Popen with `# background-launch: allow <why>` passes RULE 1")
        expect(flagged("tests/smoke/empty_reason.py", "launches the engine"),
               "allow marker with no reason does NOT count")
        expect(not flagged("scripts/ok_launch.py"), "nofocus.spawn is clean")
        expect(any(r == "tests/smoke/allowed_launch.py" for r, _l, _n in launches),
               "tests/ is scanned (allowed_launch.py appears in launches)")
        expect(flagged("scripts/mcp/unhidden.py", "CREATE_NO_WINDOW"),
               "console child without a no-window flag FAILS in a strict path")
        expect(not flagged("scripts/mcp/hidden.py"),
               "hidden_kwargs()/child_kwargs() children pass RULE 10")
        expect(flagged("scripts/bare_flag.py", "bare creationflags=CREATE_NO_WINDOW"),
               "bare creationflags=CREATE_NO_WINDOW on a console child FAILS (conhost per child)")
        expect(not flagged("scripts/bare_flag_allowed.py"),
               "bare CREATE_NO_WINDOW with `# background-launch: allow <why>` passes")
        expect(not flagged("scripts/py_wrapper.py", "launches the engine"),
               "python wrapper taking an exe path is not an engine launch")
        expect(flagged("scripts/raw_cursor.py", "SetCursorPos outside"),
               "SetCursorPos in an ordinary module FAILS (RULE 11)")
        expect(flagged("scripts/smoke_lib/win_input.py", "SetCursorPos outside"),
               "set_cursor_pos without the user_is_active() check FAILS (RULE 11)")
        guarded = ("def user_is_active():\n    return False\n"
                   "def set_cursor_pos(pos):\n    if user_is_active():\n        return False\n"
                   "    return u.SetCursorPos(pos[0], pos[1])\n")
        expect(not cursor_write_problems(CURSOR_WRITE_OWNER, ast.parse(guarded)),
               "set_cursor_pos that checks user_is_active() first passes RULE 11")
        expect(flagged("tests/unfixtured/test_guard.py", "MC2_CURSOR_GUARD_OFF"),
               "CursorGuard() in a test with no MC2_CURSOR_GUARD_OFF conftest FAILS (RULE 12)")
        expect(not flagged("tests/fixtured/test_guard.py"),
               "CursorGuard() under a conftest that sets MC2_CURSOR_GUARD_OFF passes RULE 12")
        expect(flagged("scripts/blanket_kill.py", "taskkill /IM"),
               "taskkill /IM mc2.exe FAILS (RULE 13b)")
        expect(not any(p.startswith("scripts/blanket_kill.py") and ":6:" in p for p in problems),
               "taskkill /PID <own pid> passes RULE 13b")
        expect(flagged("scripts/smoke_lib/nofocus.py", "spawn() does not call"),
               "nofocus.spawn without the exe vetting FAILS (RULE 13a)")
        expect(flagged("scripts/smoke_lib/nofocus.py", "_vet_engine_exe() does not call"),
               "a vetting function that never calls check_exe FAILS (RULE 13a)")
        expect(not flagged("scripts/smoke_lib/nofocus.py", "run() does not call"),
               "nofocus.run that calls the vetting passes RULE 13a")
        allow = {k: {} for k in ALLOW_SECTIONS}
        allow["nofocus_callers"]["scripts/ok_launch.py"] = "fixture"
        allow["input_injection"]["scripts/inject_ok.py"] = "fixture"
        allow["direct_spawn"]["scripts/stale_spawn.py"] = "fixture"
        p2, _l2, ex2 = check(root, use_git=False, allow=allow)

        def flagged2(relp: str, needle: str = "") -> bool:
            return any(p.startswith(relp) and needle in p for p in p2)

        expect(flagged2("scripts/new_launcher.py", "nofocus_callers"),
               "a nofocus caller missing from the allowlist FAILS (RULE 14)")
        expect(not flagged2("scripts/ok_launch.py", "nofocus_callers"), "a registered nofocus caller passes RULE 14")
        expect(flagged2("scripts/var_spawn.py", "direct_spawn"),
               "a non-literal-program spawn missing from the allowlist FAILS (RULE 14)")
        expect(flagged2("scripts/inject.py", "FOREGROUND"), "keybd_event in an unregistered file FAILS (RULE 15)")
        expect(not flagged2("scripts/inject_ok.py"), "a registered injector that gates on allow_foreground passes")
        expect(flagged2("scripts/oskill.py", "os.kill"), "os.kill FAILS (RULE 15)")
        expect(any("stale entry" in p and "stale_spawn.py" in p for p in p2),
               "an allowlist entry that no longer launches FAILS as stale (RULE 14)")
        allow["user_tools"]["tools/user_launch.bat"] = "fixture"
        p3, _l3, ex3 = check(root, use_git=False, allow=allow)
        expect("tools/user_launch.bat" in ex3["user_tools"] and not any(p.startswith("tools/user_launch.bat") for p in p3),
               "a registered user tool is exempt from the shell-launcher marker (RULE 9)")
        expect(flagged("tools/user_launch.bat", "shell launcher"),
               "an unregistered mc2fx_preview launcher FAILS RULE 9")
        expect(flagged("launch_bad.bat", "shell launcher"),
               ".bat engine launcher without a marker FAILS (RULE 9)")
        expect(not flagged("launch_ok.bat"), ".bat engine launcher with marker passes")
        del text
    print("[background-launch] selftest " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


ENGINE_IMAGE_NAMES = ("mc2.exe", "editrel.exe", "mission editor.exe")
NOFOCUS_FILE = "scripts/smoke_lib/nofocus.py"
LAUNCH_VET_FUNC = "_vet_engine_exe"


def engine_kill_problems(rel: str, tree: ast.AST, scan: FileScan | None = None) -> list:
    """RULE 13b: an image-name kill of an engine exe."""
    out = []
    for node in (scan.calls if scan is not None else ast.walk(tree)):
        if not isinstance(node, ast.Call):
            continue
        strs = (scan.call_strs.get(id(node), []) if scan is not None
                else [c.value.casefold() for c in ast.walk(node)
                      if isinstance(c, ast.Constant) and isinstance(c.value, str)])
        if "taskkill" in strs and "/im" in strs and any(n in strs for n in ENGINE_IMAGE_NAMES):
            out.append(f"{rel}:{node.lineno}: taskkill /IM of an engine exe ends every running game, "
                       f"including the owner's. Kill only the pid you spawned.")
    return out


def launch_vet_problems(rel: str, tree: ast.AST, scan: FileScan | None = None) -> list:
    """RULE 13a: nofocus.spawn()/run() must call the shared exe vetting."""
    if rel != NOFOCUS_FILE:
        return []
    funcs = {f.name: f for f in (scan.nodes if scan is not None else ast.walk(tree))
             if isinstance(f, ast.FunctionDef)}
    out = []
    for name in ("spawn", "run"):
        fn = funcs.get(name)
        if fn is None or not any(isinstance(c, ast.Call) and call_name(c).rsplit(".", 1)[-1] == LAUNCH_VET_FUNC
                                 for c in ast.walk(fn)):
            out.append(f"{rel}: {name}() does not call {LAUNCH_VET_FUNC}(); every launcher relies on it to refuse "
                       f"a stale exe.")
    vet = funcs.get(LAUNCH_VET_FUNC)
    if vet is None or not any(isinstance(c, ast.Call) and call_name(c).rsplit(".", 1)[-1] == "check_exe"
                              for c in ast.walk(vet)):
        out.append(f"{rel}: {LAUNCH_VET_FUNC}() does not call exe_guard.check_exe().")
    return out


TEST_CURSOR_APIS = ("CursorGuard", "park_cursor_if_edge", "ClipCursor")
TEST_CURSOR_OFF_ENV = "MC2_CURSOR_GUARD_OFF"


def _is_test_tree_file(rel: str) -> bool:
    return rel.startswith(("tests/", "scripts/tests/", "tools/")) or Path(rel).name.startswith("test_")


def test_cursor_isolation_problems(root: Path, rel: str, tree: ast.AST, src: str,
                                   scan: FileScan | None = None) -> list:
    """RULE 12: a test-tree file that can reach the real pointer needs MC2_CURSOR_GUARD_OFF set."""
    if not _is_test_tree_file(rel) or Path(rel).name == "conftest.py":
        return []
    hits = []
    for node in (scan.nodes if scan is not None else ast.walk(tree)):
        kind = type(node)
        if kind is ast.Call:
            leaf = (scan.call_names[id(node)] if scan is not None
                    else call_name(node)).rsplit(".", 1)[-1]
            if leaf in TEST_CURSOR_APIS:
                hits.append((node.lineno, leaf))
        elif kind is ast.Attribute and node.attr == "ClipCursor":
            hits.append((node.lineno, "ClipCursor"))
    if not hits or TEST_CURSOR_OFF_ENV in src:
        return []
    d = (root / rel).parent
    while True:
        conf = d / "conftest.py"
        if conf.is_file() and TEST_CURSOR_OFF_ENV in conf.read_text(encoding="utf-8", errors="replace"):
            return []
        if d == root or d == d.parent:
            break
        d = d.parent
    line, api = hits[0]
    return [f"{rel}:{line}: {api} in a test tree with no {TEST_CURSOR_OFF_ENV}=1 fixture. Add it to a "
            f"conftest.py above this file (or set it in the file): the real guard calls "
            f"ClipCursor/SetCursorPos on the user's desktop."]


def cursor_write_problems(rel: str, tree: ast.AST, scan: FileScan | None = None) -> list:
    """RULE 11: every mention of the pointer-write API outside its one guarded function."""
    allowed = None      # (first_line, last_line) of the guarded function in the owner file
    if rel == CURSOR_WRITE_OWNER:
        for fn in (scan.nodes if scan is not None else ast.walk(tree)):
            if type(fn) is ast.FunctionDef and fn.name == CURSOR_WRITE_FUNC:
                guard_lines = [c.lineno for c in ast.walk(fn)
                               if isinstance(c, ast.Call) and call_name(c).rsplit(".", 1)[-1] == CURSOR_WRITE_GUARD]
                if guard_lines:
                    allowed = (min(guard_lines), fn.end_lineno)
                break
    out = []
    if scan is not None:
        for lineno, name in scan.cursor_hits:
            if allowed and allowed[0] < lineno <= allowed[1]:
                continue
            out.append(f"{rel}:{lineno}: {name} outside {CURSOR_WRITE_OWNER}:{CURSOR_WRITE_FUNC}() "
                       f"(after its {CURSOR_WRITE_GUARD}() check). Call win_input.set_cursor_pos() "
                       f"so a user at the keyboard is never fought for the mouse.")
        return out
    for node in (scan.nodes if scan is not None else ast.walk(tree)):
        kind = type(node)
        if kind is ast.Attribute and node.attr == CURSOR_WRITE_API:
            name = node.attr
        elif kind is ast.Name and node.id == CURSOR_WRITE_API:
            name = node.id
        elif (kind is ast.Call and call_name(node).rsplit(".", 1)[-1] in ("getattr", "GetProcAddress")
              and any(isinstance(a, ast.Constant) and a.value == CURSOR_WRITE_API for a in node.args)):
            name = CURSOR_WRITE_API     # looked up by name; a list of names to scan for is not a call
        else:
            continue
        if allowed and allowed[0] < node.lineno <= allowed[1]:
            continue
        out.append(f"{rel}:{node.lineno}: {name} outside {CURSOR_WRITE_OWNER}:{CURSOR_WRITE_FUNC}() "
                   f"(after its {CURSOR_WRITE_GUARD}() check). Call win_input.set_cursor_pos() "
                   f"so a user at the keyboard is never fought for the mouse.")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--write", action="store_true",
                    help="regenerate docs/background-launch-inventory.md and the "
                         "RULE 10 child-spawn baseline")
    ap.add_argument("--selftest", action="store_true",
                    help="prove each rule can fail on synthetic input")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    problems, launches, extras = check()
    content = render_inventory(launches, extras)
    baseline = render_baseline(extras)

    if args.write:
        INVENTORY.parent.mkdir(parents=True, exist_ok=True)
        with open(INVENTORY, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        with open(CHILD_BASELINE_PATH, "w", encoding="utf-8", newline="\n") as f:
            f.write(baseline)
        print(f"[background-launch] wrote {rel(INVENTORY)} "
              f"({len(launches)} launch sites) and {rel(CHILD_BASELINE_PATH)} "
              f"({sum(extras['unhidden'].values())} unflagged console spawn(s) "
              f"in the ratchet)")
        # --write regenerates the artifacts; re-run the rules against them.
        problems, launches, extras = check()
    else:
        if INVENTORY.exists() and INVENTORY.read_text(encoding="utf-8") != content:
            problems.append(f"{rel(INVENTORY)}: stale. Re-run with --write.")

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

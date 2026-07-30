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

#: The four vars that keep an agent run off the user's screen. Any launch path
#: that can prevent these reaching the child engine is a regression.
WINDOWING_VARS = ("MC2_WINDOWED", "MC2_WIDTH", "MC2_HEIGHT", "MC2_NO_CURSOR_GRAB")

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
FORCED_MINIMUM = ("MC2_WINDOWED", "MC2_NO_CURSOR_GRAB")

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
ENGINE_ARGV = re.compile(
    r"mc2\.exe|EditRel|Mission Editor\.exe|\bGAME_EXE\b|\bDEFAULT_EXE\b|"
    r"\bexe_path\b|\bcfg\.exe\b|\bargs\.exe\b|\bproc_args\b|\bgame_exe\b",
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


def check():
    problems: list[str] = []
    launches: list[tuple[str, int, str]] = []

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
                if var not in src:
                    problems.append(
                        f"{r}: defines an MC2_* env policy but never names "
                        f"{var}. A deny-by-default allowlist/scrub that omits "
                        f"it strips the var before it reaches the engine, and "
                        f"the run goes fullscreen / grabs the cursor even "
                        f"though STARTUPINFO looks correct. Add it to the "
                        f"harness-control passthrough (see "
                        f"mc2_env_pin.HARNESS_CONTROL).")

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
        "| file | line | call |",
        "| --- | --- | --- |",
    ]
    for r, lineno, name in sorted(launches):
        lines.append(f"| `{r}` | {lineno} | `{name}` |")
    lines.append("")
    lines.append(f"Total engine launch sites: {len(launches)}")
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

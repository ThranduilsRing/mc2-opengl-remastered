#!/usr/bin/env python3
"""tools_smoke - build the optional viewport tools once, then capture one PNG from each and aggregate the verdicts.

WHAT: battery wrapper around scripts/run_tool_capture.py (the existing launch-and-screenshot harness). It optionally builds
  the opt-in targets (mc2fx_preview, mc2weapon_gui) through scripts/ninja_build.py, then runs the three captures --
  asset_viewer, mc2fx_preview, mc2weapon_gui -- and fails if any of them fails. Captures and logs are staged under
  G:/mc2-battery/tools/.
WHY: the three tools had no single command and nothing checked that the optional targets were actually built; a missing exe
  made run_tool_capture exit 2 with no aggregator around it.
USAGE (battery command line):
  py -3 tools/tools_smoke.py --build
  py -3 tools/tools_smoke.py --no-build --only mc2weapon_gui
Exit 0 = every capture PASS, 1 = at least one capture FAILED, 2 = configuration/build problem (nothing captured).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CAPTURE = ROOT / "scripts" / "run_tool_capture.py"
NINJA_BUILD = ROOT / "scripts" / "ninja_build.py"

DEFAULT_TREE = "build64-ninja"
DEFAULT_STAGE = Path("G:/mc2-battery/tools")
DEFAULT_INSTALL = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")
INSTALL_ROOT = Path("A:/Games/mc2-opengl")
FORBIDDEN_INSTALLS = ("mc2-win64-v0.4c",)

# Opt-in CMake targets (CMakeLists.txt: ENABLE_MC2FX_PREVIEW / ENABLE_MC2WEAPON_GUI gate their add_subdirectory).
OPTIONAL_TARGETS = ["mc2fx_preview", "mc2weapon_gui"]
TOOLS = ["asset_viewer", "mc2fx_preview", "mc2weapon_gui"]


def _stage_dir(raw: str) -> Path:
    """Resolve the staging dir and refuse to write inside a game install."""
    stage = Path(raw).resolve()
    try:
        stage.relative_to(INSTALL_ROOT.resolve())
    except ValueError:
        return stage
    raise ValueError(f"refusing to stage under the game install ({stage}); use {DEFAULT_STAGE}")


def _check_cwd(raw: str) -> Path:
    """Resolve the tool working dir and refuse the golden install."""
    cwd = Path(raw).resolve()
    if cwd.name.lower() in FORBIDDEN_INSTALLS:
        raise ValueError(f"refusing to use the golden install {cwd.name} as --cwd")
    return cwd


def _tree_has_optional_tools(tree_dir: Path) -> tuple[bool, str]:
    """True when the tree's CMakeCache has both opt-in tools enabled."""
    cache = tree_dir / "CMakeCache.txt"
    if not cache.is_file():
        return False, f"build tree not configured: {tree_dir}"
    try:
        text = cache.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return False, f"cannot read {cache}: {exc}"
    missing = [name for name in ("ENABLE_MC2FX_PREVIEW", "ENABLE_MC2WEAPON_GUI")
               if f"{name}:BOOL=ON" not in text]
    if missing:
        return False, (f"{tree_dir.name} is not configured with {', '.join(missing)}=ON "
                       f"(see CMakeLists.txt)")
    return True, ""


def _build(tree: str, jobs: int) -> tuple[bool, str]:
    """Build the two opt-in tool targets through ninja_build.py (it owns vcvars and MC2_NINJA_TREE)."""
    cmd = [sys.executable, str(NINJA_BUILD), f"-j{jobs}", *OPTIONAL_TARGETS]
    print(f"[tools_smoke] build: {' '.join(cmd)} (tree {tree})", flush=True)
    env = dict(os.environ)
    env["MC2_NINJA_TREE"] = tree
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT), env=env)
    except FileNotFoundError:
        return False, f"could not run {NINJA_BUILD}"
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


def _capture_argv(tool: str, out: Path, a) -> list[str]:
    """The run_tool_capture.py command line for one tool (see its module docstring)."""
    argv = [sys.executable, str(CAPTURE), tool, "--out", str(out), "--tree", a.tree]
    if a.timeout:
        argv += ["--timeout", str(a.timeout)]
    if tool == "asset_viewer":
        # The viewer resolves data/tgl/... relative to its cwd, so it runs in the install.
        argv += ["--cwd", a.cwd, "--", "--view", "props", "data/tgl/2civliving.tgl"]
    elif tool == "mc2fx_preview":
        if a.validation:
            argv += ["--validation"]
        argv += ["--", str(Path(a.cwd) / "data" / "effects" / "mc2.fx")]
    else:
        if a.validation:
            argv += ["--validation"]
    return argv


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", default=DEFAULT_TREE,
                    help=f"build tree under the repo root holding the tool exes (default {DEFAULT_TREE})")
    ap.add_argument("--out-dir", default=str(DEFAULT_STAGE),
                    help=f"staging dir for PNGs and logs (default {DEFAULT_STAGE})")
    ap.add_argument("--cwd", default=str(DEFAULT_INSTALL),
                    help="install the tools read data from (asset_viewer cwd, fx path); "
                         f"default {DEFAULT_INSTALL}")
    ap.add_argument("--only", action="append", choices=TOOLS, default=[],
                    help="capture only this tool (repeatable); default is all three")
    ap.add_argument("--build", dest="build", action="store_true", default=True,
                    help="build the opt-in targets first (default)")
    ap.add_argument("--no-build", dest="build", action="store_false",
                    help="skip the build and capture whatever is already built")
    ap.add_argument("--jobs", type=int, default=2, help="build parallelism (default 2)")
    ap.add_argument("--validation", dest="validation", action="store_true", default=True,
                    help="require '[TOOLVK] validation errors=0' for mc2fx_preview and "
                         "mc2weapon_gui (default; asset_viewer never gets it)")
    ap.add_argument("--no-validation", dest="validation", action="store_false",
                    help="do not require the validation line")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="per-tool capture timeout in seconds (default 60)")
    a = ap.parse_args()

    try:
        stage = _stage_dir(a.out_dir)
        a.cwd = str(_check_cwd(a.cwd))
    except ValueError as exc:
        print(f"tools_smoke: {exc}", file=sys.stderr)
        return 2
    stage.mkdir(parents=True, exist_ok=True)

    if a.build:
        ok, why = _tree_has_optional_tools(ROOT / a.tree)
        if not ok:
            print(f"tools_smoke: {why}. Reconfigure the tree with "
                  f"-DENABLE_MC2FX_PREVIEW=ON -DENABLE_MC2WEAPON_GUI=ON, then re-run. "
                  f"Nothing captured.", file=sys.stderr)
            return 2
        ok, text = _build(a.tree, a.jobs)
        sys.stdout.write(text)
        if not ok:
            print(f"tools_smoke: build of {', '.join(OPTIONAL_TARGETS)} failed; "
                  f"nothing captured", file=sys.stderr)
            return 2

    tools = a.only or TOOLS
    results: list[tuple[str, int]] = []
    for tool in tools:
        out = stage / f"{tool}.png"
        argv = _capture_argv(tool, out, a)
        print(f"[tools_smoke] {tool}: {' '.join(argv)}", flush=True)
        proc = subprocess.run(argv, capture_output=True, text=True, cwd=str(ROOT))
        sys.stdout.write(proc.stdout or "")
        sys.stderr.write(proc.stderr or "")
        results.append((tool, proc.returncode))

    print("\n[tools_smoke] summary")
    for tool, rc in results:
        print(f"  {tool:<16} {'PASS' if rc == 0 else 'FAIL'} (exit {rc})")
    print(f"[tools_smoke] artifacts: {stage}")

    if any(rc == 2 for _, rc in results):
        print("tools_smoke: a tool exe was missing or could not be launched "
              "(run with --build, or check --tree)", file=sys.stderr)
        return 2
    return 0 if all(rc == 0 for _, rc in results) else 1


if __name__ == "__main__":
    sys.exit(main())

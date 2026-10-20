#!/usr/bin/env python3
"""sim_trace_diff - build the two [DT] trace trees, run one mission in each, and diff the trace.

WHAT: creates isolated worktrees at 68c013fb4 (the parent of 9a8a7aee0) and HEAD
under G:/mc2-battery/simtrace-pre and G:/mc2-battery/simtrace-head, configures
each with -DMC2_SIM_TRACE=ON, builds mc2, stages each exe against an identical
disposable copy of mc2-win64-v0.4, runs the same mission twice with the fixed
determinism environment, and compares the extracted ^[DT] lines byte for byte.
WHY: the compile-time [DT] trace is the evidence for determinism-battery item 10;
the first differing line names the frame and event where the two builds diverge.

Battery command line:
  py -3 tools/sim_trace_diff.py --mission mc2_01 --duration 30

Exit codes: 0 = the [DT] streams are byte-identical; 1 = the streams differ (and
the first differing line is printed); 2 = setup/build/staging/run failure.
Nothing is launched at import time. All staging stays under G:/mc2-battery/;
the source install is only read.
"""
from __future__ import annotations

import argparse
import itertools
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BATTERY_ROOT = Path("G:/mc2-battery")
STAGE_ROOT = BATTERY_ROOT / "simtrace"
PRE_TREE = BATTERY_ROOT / "simtrace-pre"
HEAD_TREE = BATTERY_ROOT / "simtrace-head"
PRE_COMMIT = "68c013fb4"
SOURCE_INSTALL = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")
SOURCE_EXE = "mc2.exe"
BUILD_DIR = "build64-ninja"
SIM_ENV = {
    "MC2_SMOKE_FIXED_TIMESTEP": "1",
    "MC2_DETERMINISTIC_RNG": "1",
    "MC2_SIM_IDENTITY_TRACE": "1",
}
VCVARS = Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
CMAKE_CANDIDATES = (
    Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
    Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
)
FORBIDDEN_INSTALL = "mc2-win64-v0.4c"


class SetupError(RuntimeError):
    """A precondition or child command made the comparison impossible."""


def _run(argv: list[str], *, cwd: Path | None = None, env: dict | None = None,
         label: str = "") -> subprocess.CompletedProcess:
    """Run a child and turn a nonzero exit into a SetupError with a short tail."""
    shown = label or " ".join(str(a) for a in argv)
    print(f"[sim_trace_diff] {shown}", flush=True)
    try:
        proc = subprocess.run(argv, cwd=str(cwd) if cwd else None, env=env,
                              capture_output=True, text=True, errors="replace")
    except OSError as exc:
        raise SetupError(f"{shown} could not start: {exc}") from exc
    if proc.returncode:
        output = ((proc.stdout or "") + (proc.stderr or "")).splitlines()
        tail = "\n".join(output[-12:])
        raise SetupError(f"{shown} failed (rc={proc.returncode})\n{tail}")
    return proc


def _git(*args: str, cwd: Path = ROOT) -> str:
    return _run(["git", "-C", str(cwd), *args]).stdout.strip()


def _resolve_cmake() -> Path:
    for candidate in CMAKE_CANDIDATES:
        if candidate.is_file():
            return candidate
    found = shutil.which("cmake")
    if found:
        return Path(found)
    raise SetupError("cmake.exe not found in the VS2022 BuildTools/VS paths or PATH")


def _assert_disposable(path: Path) -> None:
    """Refuse to write anywhere except the dedicated G: battery root."""
    resolved = path.resolve()
    try:
        resolved.relative_to(BATTERY_ROOT.resolve())
    except ValueError as exc:
        raise SetupError(f"refusing to write outside {BATTERY_ROOT}: {resolved}") from exc


def _prepare_worktree(path: Path, commit: str) -> None:
    """Create or reuse a clean worktree at `commit`; never reset a dirty one."""
    _assert_disposable(path)
    if path.exists():
        if not (path / ".git").exists():
            raise SetupError(f"{path} exists but is not a git worktree; move it aside")
        head = _git("rev-parse", "HEAD", cwd=path)
        if head != commit:
            raise SetupError(f"{path} is at {head[:12]}, expected {commit[:12]}; "
                             "the tool will not reset an existing tree")
        dirty = _git("status", "--porcelain", "--untracked-files=no", cwd=path)
        if dirty:
            raise SetupError(f"{path} has tracked changes; the tool will not reset it")
        print(f"[sim_trace_diff] reusing worktree {path} at {head[:12]}", flush=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        _run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(path), commit],
             label=f"git worktree add {path} {commit}")
    except SetupError:
        # A failed add can leave a partial directory that would block a retry.
        if path.exists() and not (path / ".git").exists():
            shutil.rmtree(path, ignore_errors=True)
        raise


def _ensure_deps(worktree: Path) -> None:
    """Reuse bootstrap_worktree_build.ensure_deps (untracked 3rdparty binaries)."""
    if (worktree / "3rdparty" / "lib" / "x64" / "SDL2.lib").is_file():
        return
    scripts = str(ROOT / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    try:
        import bootstrap_worktree_build as bootstrap
    except ImportError as exc:
        raise SetupError(f"cannot import bootstrap_worktree_build: {exc}") from exc
    try:
        bootstrap.ensure_deps(str(worktree), str(ROOT))
    except SystemExit as exc:
        raise SetupError(f"3rdparty bootstrap exited {exc.code} for {worktree}") from exc
    if not (worktree / "3rdparty" / "lib" / "x64" / "SDL2.lib").is_file():
        raise SetupError(f"bootstrap did not populate {worktree / '3rdparty'}")


def _cmake_env() -> dict:
    """MSVC environment for a standalone cmake/ninja invocation."""
    if not VCVARS.is_file():
        raise SetupError(f"vcvars64.bat not found at {VCVARS}")
    try:
        out = subprocess.run(["cmd", "/d", "/c", f'call "{VCVARS}" >nul && set'],
                             capture_output=True, text=True, errors="replace")
    except OSError as exc:
        raise SetupError(f"could not start vcvars64.bat: {exc}") from exc
    if out.returncode:
        raise SetupError(f"vcvars64.bat failed (rc={out.returncode})")
    env = {}
    for line in out.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key] = value
    return env


def _configure_and_build(worktree: Path, cmake: Path, env: dict) -> Path:
    build_dir = worktree / BUILD_DIR
    _run([str(cmake), "-S", str(worktree), "-B", str(build_dir), "-G", "Ninja",
          "-DMC2_SIM_TRACE=ON"], env=env,
         label=f"cmake -S {worktree} -B {build_dir} -G Ninja -DMC2_SIM_TRACE=ON")
    _run([str(cmake), "--build", str(build_dir), "--target", "mc2"], env=env,
         label=f"cmake --build {build_dir} --target mc2")
    exe = build_dir / SOURCE_EXE
    if not exe.is_file():
        raise SetupError(f"build completed but {exe} is missing")
    return exe


def _copy_install(source: Path, dest: Path) -> None:
    """Make one disposable install copy; the source stays read-only."""
    _assert_disposable(dest)
    if dest.exists():
        if not dest.is_dir():
            raise SetupError(f"staging path exists and is not a directory: {dest}")
        print(f"[sim_trace_diff] reusing staged install {dest}", flush=True)
        return
    if source.name.casefold() == FORBIDDEN_INSTALL:
        raise SetupError(f"refusing to stage the golden install {source}")
    if not (source / SOURCE_EXE).is_file():
        raise SetupError(f"source install has no {SOURCE_EXE}: {source}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[sim_trace_diff] copying {source} -> {dest}", flush=True)
    try:
        # The staged exe is replaced by the trace build; skipping the source exe
        # also keeps a running install's locked binary out of the copy.
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns(SOURCE_EXE))
    except OSError as exc:
        raise SetupError(f"install copy failed: {exc}") from exc


def _stage_exe(exe: Path, install: Path) -> Path:
    """Place the built exe in the disposable install and return that path."""
    _assert_disposable(install)
    target = install / SOURCE_EXE
    try:
        shutil.copy2(exe, target)
    except OSError as exc:
        raise SetupError(f"could not stage {exe} -> {target}: {exc}") from exc
    return target


def _run_smoke(exe: Path, mission: str, duration: int) -> Path:
    """Launch through this worktree's run_smoke.py and return its artifact dir."""
    cmd = [sys.executable, str(ROOT / "scripts" / "run_smoke.py"),
           "--exe", str(exe), "--ref-exe", str(exe),
           "--mission", mission, "--duration", str(duration),
           "--sim-determinism", "--keep-logs"]
    env = dict(os.environ, **SIM_ENV)
    print(f"[sim_trace_diff] smoke {exe.parent} ({mission}, {duration}s)", flush=True)
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True,
                              text=True, errors="replace")
    except OSError as exc:
        raise SetupError(f"could not start run_smoke.py: {exc}") from exc
    output = (proc.stdout or "") + (proc.stderr or "")
    arts = [line.removeprefix("[runner] artifacts: ").strip()
            for line in output.splitlines()
            if line.startswith("[runner] artifacts: ")]
    if not arts:
        raise SetupError(f"run_smoke produced no artifact dir (rc={proc.returncode}):\n"
                         + "\n".join(output.splitlines()[-12:]))
    artifact = Path(arts[-1])
    log = artifact / f"{mission}.log"
    if not log.is_file():
        raise SetupError(f"run_smoke did not keep {log} (rc={proc.returncode})")
    if proc.returncode:
        print(f"[sim_trace_diff] note: smoke returned {proc.returncode}; the [DT] stream is still compared",
              flush=True)
    return artifact


def _extract_dt(log: Path, dest: Path) -> int:
    """Write only lines starting with [DT] and return their count."""
    count = 0
    with log.open("rb") as src, dest.open("wb") as out:
        for line in src:
            if line.startswith(b"[DT]"):
                out.write(line)
                count += 1
    return count


def _first_difference(a: Path, b: Path) -> tuple[int, str, str] | None:
    """Return (1-based line number, a line, b line) for the first differing [DT] line."""
    with a.open("rb") as fa, b.open("rb") as fb:
        for number, (left, right) in enumerate(itertools.zip_longest(fa, fb), 1):
            left = left or b""
            right = right or b""
            if left != right:
                return (number, left.rstrip(b"\r\n").decode("utf-8", "replace"),
                        right.rstrip(b"\r\n").decode("utf-8", "replace"))
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Build before/after MC2_SIM_TRACE=ON trees and diff their [DT] streams.")
    ap.add_argument("--mission", default="mc2_01", help="mission stem (default mc2_01)")
    ap.add_argument("--duration", type=int, default=30, help="mission seconds (default 30)")
    ap.add_argument("--pre-commit", default=PRE_COMMIT,
                    help=f"before commit (default {PRE_COMMIT})")
    ap.add_argument("--install", default=str(SOURCE_INSTALL),
                    help=f"read-only source install to copy (default {SOURCE_INSTALL})")
    args = ap.parse_args(argv)

    if os.name != "nt":
        raise SetupError("this battery tool requires Windows (MSVC/vcvars/Ninja)")
    if args.duration < 1 or args.duration > 60:
        raise SetupError("--duration must be 1..60 seconds")

    source_install = Path(args.install)
    pre_commit = _git("rev-parse", "--verify", f"{args.pre_commit}^{{commit}}")
    head_commit = _git("rev-parse", "HEAD")

    STAGE_ROOT.mkdir(parents=True, exist_ok=True)
    cmake = _resolve_cmake()
    env = _cmake_env()

    _prepare_worktree(PRE_TREE, pre_commit)
    _prepare_worktree(HEAD_TREE, head_commit)
    _ensure_deps(PRE_TREE)
    _ensure_deps(HEAD_TREE)

    pre_exe = _configure_and_build(PRE_TREE, cmake, env)
    head_exe = _configure_and_build(HEAD_TREE, cmake, env)

    pre_install = STAGE_ROOT / "install-pre"
    head_install = STAGE_ROOT / "install-head"
    _copy_install(source_install, pre_install)
    _copy_install(source_install, head_install)
    staged_pre = _stage_exe(pre_exe, pre_install)
    staged_head = _stage_exe(head_exe, head_install)

    pre_artifacts = _run_smoke(staged_pre, args.mission, args.duration)
    head_artifacts = _run_smoke(staged_head, args.mission, args.duration)

    pre_dt = STAGE_ROOT / "pre.dt"
    head_dt = STAGE_ROOT / "head.dt"
    pre_count = _extract_dt(pre_artifacts / f"{args.mission}.log", pre_dt)
    head_count = _extract_dt(head_artifacts / f"{args.mission}.log", head_dt)
    print(f"[sim_trace_diff] [DT] lines: pre={pre_count} head={head_count}", flush=True)
    if not pre_count or not head_count:
        raise SetupError("one or both runs emitted no [DT] lines; is MC2_SIM_TRACE in the exe?")

    difference = _first_difference(pre_dt, head_dt)
    print(f"[sim_trace_diff] pre:  {pre_artifacts}")
    print(f"[sim_trace_diff] head: {head_artifacts}")
    print(f"[sim_trace_diff] dt:   {pre_dt}  {head_dt}")
    if difference is None:
        print(f"[sim_trace_diff] PASS: {pre_count} [DT] lines are byte-identical", flush=True)
        return 0
    number, left, right = difference
    print(f"[sim_trace_diff] FAIL: first differing [DT] line {number}", flush=True)
    print(f"[sim_trace_diff] pre:  {left}")
    print(f"[sim_trace_diff] head: {right}")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SetupError as exc:
        print(f"[sim_trace_diff] ERROR: {exc}", file=sys.stderr)
        sys.exit(2)

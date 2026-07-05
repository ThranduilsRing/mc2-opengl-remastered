#!/usr/bin/env python3
"""Offline verification harness for the MC2 Vulkan shader probe.

Purpose
-------
Every Vulkan-lib slice (VULKAN-BACKEND-SKELETON-1, VULKAN-SHADER-TOOLCHAIN-1,
VULKAN-FULLSCREEN-TRIANGLE-1, VULKAN-DESCRIPTOR-SMOKE-1, VMA integration, ...)
is verified the same way: configure with MC2_VULKAN=ON, build the standalone
``vulkan_shader_probe`` target, run it against the compiled SPIR-V, and confirm
every probe token is 1 with zero validation-layer errors. This script wraps
that so nobody hand-types cmake + probe invocations (and so the checks stay
consistent).

IMPORTANT: MC2_VULKAN builds are ISOLATED in ``build64_vulkan`` -- they pull in
the Vulkan SDK loader/headers and must NEVER share the normal ``build64`` tree.
This script defaults its build dir to ``build64_vulkan`` for exactly that
reason. Do not point it at ``build64``.

What it verifies
----------------
- Build of target ``vulkan_shader_probe`` succeeds.
- Probe stdout reports ``caps=1 shaders=1 triangle=1 descriptors=1 failSoftOK=1``.
- When validation is enabled (default), zero validation-layer error lines are
  emitted. The skeleton's debug-utils messenger logs these to stderr as
  ``[VULKAN_SKELETON] desc-probe: VALIDATION: ...`` / ``... img-probe: VALIDATION: ...``.

Exit code: 0 = build OK + all probe tokens == 1 + zero validation errors;
non-zero otherwise.

Example invocations
-------------------
    # Full default run (auto-configure if needed, core validation on):
    py -3 scripts/run_vulkan_probe.py

    # Rebuild + run in the isolated Vulkan build dir, sync validation:
    py -3 scripts/run_vulkan_probe.py --validation sync

    # Skip validation layer (fast wiring smoke only):
    py -3 scripts/run_vulkan_probe.py --validation off

    # Force a reconfigure (e.g. after CMakeLists edits):
    py -3 scripts/run_vulkan_probe.py --configure

    # Build only, don't run the probe:
    py -3 scripts/run_vulkan_probe.py --no-run

Notes
-----
- Pure stdlib, Windows-friendly.
- Uses the canonical VS CMake path, falling back to ``cmake`` on PATH.
- The MC2_VULKAN_VALIDATION env var is checked by the probe with getenv() !=
  nullptr (presence, not value), so "off" simply leaves it unset; any other
  choice sets it to that string to make intent visible in logs.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Canonical VS-bundled CMake (matches CLAUDE.md). Falls back to PATH if absent.
CANONICAL_CMAKE = (
    r"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/"
    r"Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
)

# Probe success tokens (see tools/vulkan_shader_probe_main.cpp). ALL must be 1.
# VK-BOOTSTRAP-INTEGRATE-1 added "swapchain" (create+query+destroy, no present).
# VULKAN-EDGEFOG-SYNTHETIC-FIXTURE-1 added "edgefog_fixture" (shader-math oracle:
# runs the shipped edge_fog .spv on a known input, checks GPU vs a CPU reference).
SUCCESS_TOKENS = ("caps", "shaders", "triangle", "descriptors", "swapchain", "edgefog_fixture", "oobfog_fixture", "failSoftOK")

# Validation-layer error signature emitted by the skeleton's debug messenger
# (vulkan_backend_skeleton.cpp: "[VULKAN_SKELETON] <probe>: VALIDATION: <msg>").
# Match ONLY that exact debug-callback prefix ("<probe>: VALIDATION: ..."), not
# the informational "validation layer ... enabled" / "validation active" lines
# the probes also print -- those are not errors and must not inflate the count.
VALIDATION_ERROR_RE = re.compile(r":\s*VALIDATION:")

PROBE_TIMEOUT_S = 120


def worktree_root() -> Path:
    """Resolve the worktree root from this script's location (scripts/..)."""
    return Path(__file__).resolve().parent.parent


def find_cmake() -> str | None:
    if Path(CANONICAL_CMAKE).exists():
        return CANONICAL_CMAKE
    found = shutil.which("cmake")
    if found:
        return found
    return None


def find_probe_exe(build_dir: Path, config: str, target: str) -> Path | None:
    """Locate the built probe exe. Multi-config VS puts it under <cfg>/."""
    candidates = [
        build_dir / config / (target + ".exe"),
        build_dir / (target + ".exe"),
        build_dir / config / target,  # non-windows / no-suffix fallback
        build_dir / target,
    ]
    for c in candidates:
        if c.exists():
            return c
    # Last resort: recursive search (handles unexpected generator layouts).
    for match in build_dir.rglob(target + ".exe"):
        return match
    return None


def run_cmd(cmd: list[str], cwd: Path, timeout: int | None = None):
    """Run a command, capturing stdout+stderr. Never raises on nonzero exit."""
    print("  $ " + " ".join(cmd))
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout, proc.stderr, None
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or ""
        err = exc.stderr or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        if isinstance(err, bytes):
            err = err.decode("utf-8", "replace")
        return None, out, err, f"timeout after {timeout}s"
    except OSError as exc:
        return None, "", "", f"failed to launch: {exc}"


def tail(text: str, n: int = 40) -> str:
    lines = text.splitlines()
    return "\n".join(lines[-n:])


def fail(msg: str, captured: str = "") -> int:
    print("RESULT FAIL " + msg)
    if captured.strip():
        print("---- captured output (tail) ----")
        print(tail(captured))
        print("--------------------------------")
    return 1


def main(argv: list[str]) -> int:
    root = worktree_root()

    parser = argparse.ArgumentParser(
        description="Offline verification harness for the MC2 Vulkan shader probe.",
    )
    parser.add_argument(
        "--build-dir",
        default="build64_vulkan",
        help="Build directory (relative to worktree root or absolute). "
        "Default build64_vulkan -- MC2_VULKAN builds are ISOLATED here, "
        "NEVER build64.",
    )
    parser.add_argument("--config", default="RelWithDebInfo", help="CMake config (default RelWithDebInfo).")
    parser.add_argument(
        "--shader-dir",
        default=None,
        help="Directory holding the COMPILED .spv files, passed to the probe. "
        "Default: <build-dir>/shaders/vulkan -- that is where the "
        "mc2_vulkan_shaders CMake target emits fullscreen.{vert,frag}.spv. "
        "(The source .vert/.frag live in shaders/vulkan/ but the probe needs "
        "the compiled .spv, so the default MUST be the build output, not source.)",
    )
    parser.add_argument(
        "--validation",
        choices=["off", "core", "sync", "gpu-assisted", "best-practices", "debug-printf"],
        default="core",
        help="Validation mode -> MC2_VULKAN_VALIDATION. 'off' leaves it unset; "
        "any other value is exported (presence enables the layer).",
    )
    parser.add_argument("--target", default="vulkan_shader_probe", help="CMake target to build/run.")

    cfg_group = parser.add_mutually_exclusive_group()
    cfg_group.add_argument(
        "--configure",
        dest="configure",
        action="store_true",
        default=None,
        help="Force cmake configure before build.",
    )
    cfg_group.add_argument(
        "--no-configure",
        dest="configure",
        action="store_false",
        help="Skip configure even if no CMakeCache.txt exists.",
    )

    run_group = parser.add_mutually_exclusive_group()
    run_group.add_argument("--run", dest="run", action="store_true", default=True, help="Run the probe (default).")
    run_group.add_argument("--no-run", dest="run", action="store_false", help="Build only; do not run the probe.")

    args = parser.parse_args(argv)

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (root / build_dir).resolve()

    # Default the SPIR-V dir to the build output (where mc2_vulkan_shaders emits
    # the compiled .spv). Only when the user did not pass --shader-dir.
    if args.shader_dir is None:
        shader_dir = str((build_dir / "shaders" / "vulkan"))
    else:
        shader_dir = args.shader_dir
    args.shader_dir = shader_dir

    print(f"[run_vulkan_probe] worktree root : {root}")
    print(f"[run_vulkan_probe] build dir      : {build_dir}")
    print(f"[run_vulkan_probe] config         : {args.config}")
    print(f"[run_vulkan_probe] target         : {args.target}")
    print(f"[run_vulkan_probe] shader dir     : {args.shader_dir}")
    print(f"[run_vulkan_probe] validation     : {args.validation}")

    cmake = find_cmake()
    if cmake is None:
        return fail(
            "cmake not found (canonical VS path missing and 'cmake' not on PATH). "
            f"Expected: {CANONICAL_CMAKE}"
        )
    print(f"[run_vulkan_probe] cmake          : {cmake}")

    # ---- Decide whether to configure ---------------------------------------
    cache = build_dir / "CMakeCache.txt"
    if args.configure is None:
        do_configure = not cache.exists()
        print(f"[run_vulkan_probe] configure(auto): {do_configure} (cache exists={cache.exists()})")
    else:
        do_configure = args.configure
        print(f"[run_vulkan_probe] configure      : {do_configure} (explicit)")

    # ---- Configure ----------------------------------------------------------
    if do_configure:
        prefix = (root / "3rdparty").resolve()
        cfg_cmd = [
            cmake,
            "-S", str(root),
            "-B", str(build_dir),
            "-G", "Visual Studio 17 2022",
            "-A", "x64",
            "-DMC2_VULKAN=ON",
            "-DCMAKE_LIBRARY_ARCHITECTURE=x64",
            f"-DCMAKE_PREFIX_PATH={prefix}",
        ]
        print("[run_vulkan_probe] configuring...")
        rc, out, err, launch_err = run_cmd(cfg_cmd, cwd=root)
        combined = out + "\n" + err
        if launch_err is not None:
            return fail(f"cmake configure {launch_err}", combined)
        if rc != 0:
            return fail(f"cmake configure exited {rc}", combined)

    # ---- Build --------------------------------------------------------------
    build_cmd = [
        cmake,
        "--build", str(build_dir),
        "--config", args.config,
        "--target", args.target,
    ]
    print("[run_vulkan_probe] building...")
    rc, out, err, launch_err = run_cmd(build_cmd, cwd=root)
    build_output = out + "\n" + err
    if launch_err is not None:
        return fail(f"cmake build {launch_err}", build_output)
    if rc != 0:
        return fail(f"cmake build (target {args.target}) exited {rc}", build_output)
    print("[run_vulkan_probe] build PASS")

    if not args.run:
        print(f"RESULT ok build=PASS probe=SKIPPED (--no-run) validation_errors=n/a")
        return 0

    # ---- Locate probe exe ---------------------------------------------------
    exe = find_probe_exe(build_dir, args.config, args.target)
    if exe is None:
        return fail(
            f"probe exe '{args.target}' not found under {build_dir} "
            f"(config {args.config}). Build may have produced no output.",
            build_output,
        )
    print(f"[run_vulkan_probe] probe exe      : {exe}")

    # ---- Run probe ----------------------------------------------------------
    env = os.environ.copy()
    if args.validation == "off":
        env.pop("MC2_VULKAN_VALIDATION", None)
    else:
        env["MC2_VULKAN_VALIDATION"] = args.validation

    print(f"[run_vulkan_probe] running probe (shader-dir={args.shader_dir}, timeout={PROBE_TIMEOUT_S}s)...")
    print("  $ " + str(exe) + " " + args.shader_dir)
    try:
        proc = subprocess.run(
            [str(exe), args.shader_dir],
            cwd=str(root),
            capture_output=True,
            text=True,
            timeout=PROBE_TIMEOUT_S,
            env=env,
        )
        probe_rc = proc.returncode
        probe_out = proc.stdout or ""
        probe_err = proc.stderr or ""
    except subprocess.TimeoutExpired as exc:
        po = exc.stdout or ""
        pe = exc.stderr or ""
        if isinstance(po, bytes):
            po = po.decode("utf-8", "replace")
        if isinstance(pe, bytes):
            pe = pe.decode("utf-8", "replace")
        return fail(f"probe timed out after {PROBE_TIMEOUT_S}s", po + "\n" + pe)
    except OSError as exc:
        return fail(f"probe failed to launch: {exc}", build_output)

    combined = probe_out + "\n" + probe_err

    # ---- Parse success tokens (from stdout) ---------------------------------
    token_vals: dict[str, str] = {}
    for tok in SUCCESS_TOKENS:
        m = re.search(rf"\b{re.escape(tok)}=(\d+)", probe_out)
        token_vals[tok] = m.group(1) if m else "?"

    tokens_ok = all(token_vals[t] == "1" for t in SUCCESS_TOKENS)

    # ---- Count validation errors (from combined stdout+stderr) --------------
    # The debug messenger logs each caught validation error/warning on its own
    # line containing "VALIDATION". Only meaningful when validation is enabled.
    if args.validation == "off":
        validation_errors = 0
    else:
        validation_errors = sum(
            1 for line in combined.splitlines() if VALIDATION_ERROR_RE.search(line)
        )

    # Compact per-token summary: caps1/shaders1/triangle1/desc1/failsoft1
    short_names = {
        "caps": "caps",
        "shaders": "shaders",
        "triangle": "triangle",
        "descriptors": "desc",
        "swapchain": "swapchain",
        "edgefog_fixture": "edgefog",
        "oobfog_fixture": "oobfog",
        "failSoftOK": "failsoft",
    }
    probe_summary = "/".join(f"{short_names[t]}{token_vals[t]}" for t in SUCCESS_TOKENS)

    overall_ok = tokens_ok and probe_rc == 0 and validation_errors == 0

    status = "ok" if overall_ok else "FAIL"
    print(
        f"RESULT {status} build=PASS probe={probe_summary} "
        f"probe_exit={probe_rc} validation_errors={validation_errors}"
    )

    if not overall_ok:
        reasons = []
        if not tokens_ok:
            reasons.append("not all probe tokens == 1")
        if probe_rc != 0:
            reasons.append(f"probe exit {probe_rc}")
        if validation_errors:
            reasons.append(f"{validation_errors} validation error line(s)")
        print("  reasons: " + "; ".join(reasons))
        print("---- probe output (tail) ----")
        print(tail(combined))
        print("-----------------------------")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
# scripts/run_smoke_asan.py
"""MC2 Tier 4 -- AddressSanitizer smoke gate.

Single-mission smoke (default mc2_01, 30s) under an MSVC /fsanitize=address
build of mc2.exe. The ASan binary lives in build64-asan/ and is launched from
a sibling deploy dir (mc2-win64-asan/) so it never clobbers the production
build64/ or mc2-win64-v0.4/ binary.

Prerequisites:
  1. Configure: cmake -S . -B build64-asan -DMC2_ASAN=ON ... (see
     docs/asan-mvp-runbook.md for the exact GLEW/ZLIB path overrides).
  2. Build:     cmake --build build64-asan --config RelWithDebInfo --target mc2
  3. Data tree: the script auto-mirrors the data tree by reusing the
     mc2-win64-v0.4/ install as CWD and copying the ASan exe + runtime DLL
     in alongside under the name mc2-asan.exe. The production mc2.exe is
     never touched (per asan-mvp-runbook.md "DO NOT overwrite" rule).

Exit codes:
  0 -- clean: process exited and no ASan error markers in stderr
  1 -- launch / setup failure (build missing, exe failed to spawn, etc.)
  2 -- ASan tripped (one or more "ERROR: AddressSanitizer" / SUMMARY lines)

Run:
  py -3 scripts/run_smoke_asan.py
  py -3 scripts/run_smoke_asan.py --mission mc2_01 --duration 30
  py -3 scripts/run_smoke_asan.py --suppressions scripts/asan_suppressions.txt
"""
from __future__ import annotations

import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from smoke_lib import nofocus  # noqa: E402

ASAN_BUILD_DIR = ROOT / "build64-asan" / "RelWithDebInfo"
DEPLOY_SRC = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4")
ASAN_EXE_NAME = "mc2-asan.exe"
ASAN_RUNTIME_DLL = "clang_rt.asan_dynamic-x86_64.dll"
DEFAULT_MISSION = "mc2_01"
DEFAULT_DURATION = 30

ERROR_PATTERNS = [
    re.compile(r"==\d+==ERROR: AddressSanitizer", re.IGNORECASE),
    re.compile(r"^SUMMARY: AddressSanitizer:", re.IGNORECASE | re.MULTILINE),
]


def _running_mc2_asan() -> list[int]:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {ASAN_EXE_NAME}", "/NH", "/FO", "CSV"],
            text=True, stderr=subprocess.DEVNULL,
            **nofocus.hidden_kwargs())
    except Exception:
        return []
    pids = []
    for line in out.splitlines():
        if ASAN_EXE_NAME in line:
            parts = [p.strip('"') for p in line.split(",")]
            if len(parts) > 1 and parts[1].isdigit():
                pids.append(int(parts[1]))
    return pids


def _deploy_asan_binary(src_dir: Path, dst_dir: Path) -> tuple[Path, Path]:
    """Copy mc2.exe -> mc2-asan.exe and the runtime DLL into dst_dir.

    Uses cp -f semantics (per-file copy + diff verification) per project
    policy. Returns (exe_path, dll_path) in dst_dir.
    """
    src_exe = src_dir / "mc2.exe"
    src_dll = src_dir / ASAN_RUNTIME_DLL
    if not src_exe.is_file():
        raise SystemExit(f"[asan-smoke] ASan exe missing: {src_exe}\n"
                         f"  Build first: cmake --build build64-asan "
                         f"--config RelWithDebInfo --target mc2")
    if not src_dll.is_file():
        raise SystemExit(f"[asan-smoke] ASan runtime DLL missing: {src_dll}")

    dst_exe = dst_dir / ASAN_EXE_NAME
    dst_dll = dst_dir / ASAN_RUNTIME_DLL

    shutil.copy2(src_exe, dst_exe)
    shutil.copy2(src_dll, dst_dll)
    # Verify (no silent fallback like cp -r).
    if not filecmp.cmp(src_exe, dst_exe, shallow=False):
        raise SystemExit(f"[asan-smoke] exe copy verify failed: {dst_exe}")
    if not filecmp.cmp(src_dll, dst_dll, shallow=False):
        raise SystemExit(f"[asan-smoke] dll copy verify failed: {dst_dll}")
    return dst_exe, dst_dll


def _build_asan_options(suppressions: Path | None) -> str:
    opts = [
        "detect_leaks=0",
        "abort_on_error=1",
        "halt_on_error=1",
        "print_stacktrace=1",
        "symbolize=1",
    ]
    if suppressions and suppressions.is_file():
        opts.append(f"suppressions={suppressions.resolve()}")
    return ":".join(opts)


def _scan_for_errors(text: str) -> list[str]:
    hits = []
    for pat in ERROR_PATTERNS:
        for m in pat.finditer(text):
            hits.append(m.group(0).strip())
    return hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mission", default=DEFAULT_MISSION,
                    help=f"Mission to load (default: {DEFAULT_MISSION}). "
                         f"Tier1 (5 missions) is too slow under ASan.")
    ap.add_argument("--duration", type=int, default=DEFAULT_DURATION,
                    help=f"Seconds to run before SIGTERM (default: {DEFAULT_DURATION})")
    ap.add_argument("--build-dir", default=str(ASAN_BUILD_DIR),
                    help="ASan build output dir (default: build64-asan/RelWithDebInfo)")
    ap.add_argument("--deploy-dir", default=str(DEPLOY_SRC),
                    help="Existing mc2 install to reuse as CWD/data tree "
                         "(default: mc2-win64-v0.4). mc2-asan.exe lands here; "
                         "production mc2.exe is NOT touched.")
    ap.add_argument("--suppressions", default=None,
                    help="Optional ASan suppressions file (none by default)")
    ap.add_argument("--kill-existing", action="store_true",
                    help="Taskkill any running mc2-asan.exe before launch")
    ap.add_argument("--keep-log", action="store_true",
                    help="Keep stderr log alongside ASan exe (asan-mc2_XX.log)")
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    deploy_dir = Path(args.deploy_dir)
    suppressions = Path(args.suppressions) if args.suppressions else None

    if not deploy_dir.is_dir():
        print(f"[asan-smoke] ERROR: deploy dir missing: {deploy_dir}",
              file=sys.stderr)
        return 1

    if args.kill_existing:
        pids = _running_mc2_asan()
        if pids:
            print(f"[asan-smoke] killing existing {ASAN_EXE_NAME} PIDs {pids}",
                  file=sys.stderr)
            subprocess.run(["taskkill", "/F", "/IM", ASAN_EXE_NAME],
                           stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           **nofocus.hidden_kwargs())
            time.sleep(0.5)
    else:
        pids = _running_mc2_asan()
        if pids:
            print(f"[asan-smoke] ERROR: {ASAN_EXE_NAME} already running "
                  f"(PIDs {pids}); pass --kill-existing", file=sys.stderr)
            return 1

    try:
        exe_path, _dll_path = _deploy_asan_binary(build_dir, deploy_dir)
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        return 1

    env = os.environ.copy()
    env["ASAN_OPTIONS"] = _build_asan_options(suppressions)
    # Mirror run_smoke.py: avoid carrying instrumentation env vars that would
    # bloat output; run_smoke_asan focuses on memory bugs, not [SUBSYS] traces.
    for var in ["MC2_SMOKE_MODE", "MC2_HEARTBEAT", "MC2_SMOKE_SEED"]:
        env.pop(var, None)

    log_path = deploy_dir / f"asan-{args.mission}.log"
    if args.keep_log and log_path.exists():
        log_path.unlink()

    print(f"[asan-smoke] launch {exe_path.name} mission={args.mission} "
          f"duration={args.duration}s cwd={deploy_dir}",
          file=sys.stderr)
    print(f"[asan-smoke] ASAN_OPTIONS={env['ASAN_OPTIONS']}", file=sys.stderr)

    start_wall = time.monotonic()
    proc = nofocus.spawn(
        [str(exe_path), "-mission", args.mission],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=str(deploy_dir),
        env=env,
    )

    stderr_chunks: list[str] = []
    deadline = time.monotonic() + args.duration
    try:
        while time.monotonic() < deadline:
            rc = proc.poll()
            if rc is not None:
                break
            time.sleep(0.5)
        if proc.poll() is None:
            print(f"[asan-smoke] duration {args.duration}s elapsed; "
                  f"terminating", file=sys.stderr)
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    finally:
        if proc.stdout:
            try:
                stderr_chunks.append(proc.stdout.read() or "")
            except Exception:
                pass

    elapsed = time.monotonic() - start_wall
    output = "".join(stderr_chunks)
    if args.keep_log:
        try:
            log_path.write_text(output, encoding="utf-8", errors="replace")
            print(f"[asan-smoke] log written: {log_path}", file=sys.stderr)
        except Exception as e:
            print(f"[asan-smoke] log write failed: {e}", file=sys.stderr)

    errors = _scan_for_errors(output)
    rc = proc.returncode if proc.returncode is not None else -1
    print(f"[asan-smoke] mission={args.mission} elapsed={elapsed:.1f}s "
          f"exit={rc} asan_errors={len(errors)}", file=sys.stderr)

    if errors:
        print("[asan-smoke] ASan error markers (first 5):", file=sys.stderr)
        for line in errors[:5]:
            print(f"  {line}", file=sys.stderr)
        # Tail of output for context.
        tail = "\n".join(output.splitlines()[-40:])
        print(f"[asan-smoke] --- output tail ---\n{tail}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())

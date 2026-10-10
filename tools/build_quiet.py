#!/usr/bin/env python3
"""build_quiet.py — VS CMake build with the spew swallowed.

Runs the canonical VS CMake build and prints ONLY: error lines verbatim,
warning COUNT, the final "*.vcxproj -> <output>" line, and elapsed time.
Exit code = cmake's exit code.

    py -3 tools/build_quiet.py [--worktree <dir>] [--build-dir build64] \
        [--config RelWithDebInfo] [--target mc2] [--clean-first]

Defaults match the repo canon: RelWithDebInfo (Release crashes on GL debug
callback registration), build dir build64 under the worktree, explicit VS
BuildTools CMake path (cmake is usually not on PATH).
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

CMAKE = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/"
         "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--worktree", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--build-dir", default="build64")
    ap.add_argument("--config", default="RelWithDebInfo")
    ap.add_argument("--target", default="mc2")
    ap.add_argument("--clean-first", action="store_true")
    ap.add_argument("--cmake", default=CMAKE, help="cmake executable path")
    args = ap.parse_args()

    build_dir = Path(args.worktree) / args.build_dir
    cmd = [args.cmake, "--build", str(build_dir), "--config", args.config,
           "--target", args.target]
    if args.clean_first:
        cmd.append("--clean-first")

    print(f"[build_quiet] {' '.join(cmd)}", flush=True)
    t0 = time.time()
    errors, vcx_lines = [], []
    nwarn = 0
    err_rx = re.compile(r"error", re.IGNORECASE)
    warn_rx = re.compile(r"warning", re.IGNORECASE)
    vcx_rx = re.compile(r"\.vcxproj\s*->\s*")

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", **console_child.child_kwargs(redirected=True))
    for line in proc.stdout:
        line = line.rstrip()
        if err_rx.search(line):
            errors.append(line)
        elif warn_rx.search(line):
            nwarn += 1
        if vcx_rx.search(line):
            vcx_lines.append(line.strip())
    rc = proc.wait()
    elapsed = time.time() - t0

    for e in errors:
        print(e)
    print(f"[build_quiet] warnings: {nwarn}")
    if vcx_lines:
        print(f"[build_quiet] {vcx_lines[-1]}")
    print(f"[build_quiet] exit={rc} elapsed={elapsed:.1f}s")
    return rc


if __name__ == "__main__":
    sys.exit(main())

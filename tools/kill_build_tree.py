#!/usr/bin/env python3
"""kill_build_tree - end the build processes of ONE build directory and nothing else.

WHAT: ends ninja, cl, link and the other compiler/linker processes whose working directory or command line is under the given build
directory. Never matches by image name alone, never touches this process or its ancestors.
WHY: lane 2 builds on the same host; `Stop-Process cl,ninja` (the only quick way to stop a runaway tidy proof) also ended its compiles.
A build tree is the unit that identifies whose compiler it is.

  py -3 tools/kill_build_tree.py build-alltargets           # end this tree's build processes
  py -3 tools/kill_build_tree.py build-alltargets --dry-run  # list them
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

#: Image names that belong to a build; anything else (a shell, an editor, the game) is never a victim.
BUILD_IMAGES = frozenset({"ninja.exe", "cl.exe", "link.exe", "lib.exe", "rc.exe", "mt.exe", "cvtres.exe", "cmake.exe", "ctest.exe",
                          "clang-tidy.exe", "clang.exe", "clang-cl.exe", "msbuild.exe", "vctip.exe", "mspdbsrv.exe"})


def _norm(p: str) -> str:
    return os.path.normpath(p).replace("\\", "/").casefold() if p else ""


def under(path: str, build_dir: str) -> bool:
    p, b = _norm(path), _norm(build_dir)
    return bool(p) and (p == b or p.startswith(b + "/"))


def mentions(cmdline: str, build_dir: str) -> bool:
    """True when the command line names the build directory (as a path, either slash)."""
    b = _norm(build_dir)
    c = (cmdline or "").replace("\\", "/").casefold()
    i = c.find(b)
    while i != -1:
        j = i + len(b)
        if j == len(c) or c[j] in "/\"' ,;":        # `build-x` must not match inside `build-x2`
            return True
        i = c.find(b, j)
    return False


def select_victims(procs, build_dir: str, protect=frozenset(), images=BUILD_IMAGES):
    """procs: dicts with pid, name, cmdline, cwd. -> the ones that belong to `build_dir`. Pure: no process is touched."""
    out = []
    for p in procs:
        if p["pid"] in protect:
            continue
        if images is not None and (p.get("name") or "").casefold() not in images:
            continue
        if under(p.get("cwd") or "", build_dir) or mentions(p.get("cmdline") or "", build_dir):
            out.append(p)
    return out


def list_processes():
    """-> [{pid, name, cmdline, cwd}] for every process this user can inspect."""
    try:
        import psutil
    except ImportError:
        return _list_via_cim()
    out = []
    for pr in psutil.process_iter(["pid", "name", "cmdline", "cwd"]):
        info = pr.info
        out.append({"pid": info["pid"], "name": info.get("name") or "", "cmdline": " ".join(info.get("cmdline") or []),
                    "cwd": info.get("cwd") or ""})
    return out


def _list_via_cim():
    """Command lines only (no working directory) from WMI, when psutil is not installed."""
    ps = "Get-CimInstance Win32_Process | Select-Object ProcessId,Name,CommandLine | ConvertTo-Json -Compress"
    raw = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], capture_output=True, text=True,
                         errors="replace", **console_child.child_kwargs(redirected=True)).stdout
    data = json.loads(raw or "[]")
    return [{"pid": d["ProcessId"], "name": d.get("Name") or "", "cmdline": d.get("CommandLine") or "", "cwd": ""}
            for d in (data if isinstance(data, list) else [data])]


def own_and_ancestors() -> set:
    pids = {os.getpid()}
    try:
        import psutil
        pids |= {p.pid for p in psutil.Process().parents()}
    except Exception:
        pids.add(os.getppid())
    return pids


def kill(build_dir: str, *, images=BUILD_IMAGES, dry_run: bool = False, protect=None, procs=None):
    """End the build processes of `build_dir`. -> the victims (dicts); with dry_run nothing is ended."""
    build_dir = os.path.abspath(build_dir)
    victims = select_victims(procs if procs is not None else list_processes(), build_dir,
                             protect if protect is not None else own_and_ancestors(), images)
    if not dry_run:
        import psutil
        for v in victims:
            try:
                psutil.Process(v["pid"]).kill()
            except psutil.Error:
                pass                                   # already gone
    return victims


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build_dir", help="the build directory whose processes to end (relative to the repo root, or absolute)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--image", action="append", default=[], help="also end this image name (default set: compilers, linkers, ninja)")
    args = ap.parse_args(argv)
    bd = args.build_dir if os.path.isabs(args.build_dir) else os.path.join(ROOT, args.build_dir)
    victims = kill(bd, images=BUILD_IMAGES | {i.casefold() for i in args.image}, dry_run=args.dry_run)
    for v in victims:
        print(f"{'WOULD END' if args.dry_run else 'ended'} pid {v['pid']} {v['name']}")
    print(f"kill_build_tree: {len(victims)} process(es) of {bd}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

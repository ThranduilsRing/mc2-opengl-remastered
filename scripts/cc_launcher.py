#!/usr/bin/env python3
"""cc_launcher.py -- compiler launcher for the shared compile cache: keeps Ninja's header dependencies pointing at THIS checkout.

usage (set up by configure_ninja.cache_cmake_args):  python -S -E cc_launcher.py <this checkout root> <sccache.exe> <cl.exe> <cl args...>

WHY: Ninja learns an object's headers from the "Note: including file: <path>" lines cl.exe prints under /showIncludes. On a cache
hit sccache does not run cl.exe, it REPLAYS the stdout stored with the entry, and the entry was written by whichever checkout
compiled first. SCCACHE_BASEDIRS makes the cache KEY ignore the checkout root (that is what lets worktrees share entries) but it
does not touch the stored stdout. So a hit in worktree X recorded the first worktree's header paths (and its build directory's
generated headers) in X's .ninja_deps: an edit to X's own header never rebuilt the object (silently stale), and a path that no
longer exists made the object rebuild on every build (a deps path that is missing counts as dirty).

This launcher runs sccache, rewrites every replayed include path outside this checkout onto its counterpart here (root-agnostic:
the longest trailing path that exists under this checkout, so any former worktree or build root works), and passes everything
else (diagnostics, return code) through untouched. A path with no counterpart here (system/SDK headers) is left as printed.
"""
import os
import subprocess
import sys

import console_child  # same directory: python puts the script dir on sys.path even under -S -E

PREFIX = b"Note: including file:"


def norm(p):
    return os.path.normcase(os.path.normpath(p))


def include_dirs(args):
    dirs = []
    it = iter(args)
    for a in it:
        if a in ("/I", "-I"):
            a = next(it, "")
        elif a[:2] in ("/I", "-I"):
            a = a[2:]
        else:
            continue
        if a:
            dirs.append(a)
    return dirs


def remap(path, own, incs):
    """path: one replayed include path (str). Returns the path in this checkout, or path unchanged when none is found.

    Root-agnostic: a path not under this checkout is mapped by its longest trailing part that exists here (>= 2 components
    under the checkout root, any length under this command line's own -I directories for generated headers). System and SDK
    headers have no counterpart and stay as printed."""
    path = path.replace("\\", "/")
    own_n = norm(own) + os.sep
    n = norm(path)
    if n.startswith(own_n):
        return path
    parts = [p for p in path.split("/") if p]
    if len(parts) > 1 and path[1:2] == ":":
        parts = parts[1:]
    for i in range(len(parts)):
        sub = os.sep.join(parts[i:])
        if len(parts) - i >= 2:
            cand = os.path.join(own, sub)
            if os.path.exists(cand):
                return cand
        for d in incs:
            cand = os.path.join(d, sub)
            if os.path.exists(cand) and norm(cand).startswith(own_n):
                return cand
    return path


def main():
    own, sccache, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
    incs = include_dirs(cmd)
    proc = subprocess.run([sccache] + cmd, stdout=subprocess.PIPE, **console_child.child_kwargs(redirected=True))
    out = proc.stdout
    if PREFIX in out:
        lines = out.split(b"\n")
        for i, ln in enumerate(lines):
            if ln.startswith(PREFIX):
                body = ln[len(PREFIX):]
                lead = body[:len(body) - len(body.lstrip(b" "))]
                stripped = body.strip(b" \r")
                try:
                    path = stripped.decode("mbcs")
                except UnicodeDecodeError:
                    continue
                new = remap(path, own, incs)
                if new != path:
                    eol = b"\r" if ln.endswith(b"\r") else b""
                    lines[i] = PREFIX + lead + new.encode("mbcs") + eol
        out = b"\n".join(lines)
    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""cc_launcher.py -- compiler launcher for the shared compile cache: keeps Ninja's header dependencies pointing at THIS checkout.

usage (set up by configure_ninja.cache_cmake_args):  python -S -E cc_launcher.py <this checkout root> <sccache.exe> <cl.exe> <cl args...>

WHY: Ninja learns an object's headers from the "Note: including file: <path>" lines cl.exe prints under /showIncludes. On a cache
hit sccache does not run cl.exe, it REPLAYS the stdout stored with the entry, and the entry was written by whichever checkout
compiled first. SCCACHE_BASEDIRS makes the cache KEY ignore the checkout root (that is what lets worktrees share entries) but it
does not touch the stored stdout. So a hit in worktree X recorded the first worktree's header paths (and its build directory's
generated headers) in X's .ninja_deps: an edit to X's own header never rebuilt the object (silently stale), and a path that no
longer exists made the object rebuild on every build (a deps path that is missing counts as dirty).

This launcher runs sccache, rewrites every replayed include path that belongs to another checkout onto this one, and passes
everything else (diagnostics, return code) through untouched. A path with no counterpart here is left as printed: the object then
rebuilds every time, which is slow but never stale.
"""
import os
import subprocess
import sys

PREFIX = b"Note: including file:"


def norm(p):
    return os.path.normcase(os.path.normpath(p))


def roots_from_env(own):
    """Every checkout root the cache may have been filled from, own root excluded. Forward-slash, no trailing slash."""
    own_n = norm(own)
    roots = []
    for r in os.environ.get("SCCACHE_BASEDIRS", "").split(";"):
        r = r.strip().rstrip("/\\")
        if r and norm(r) != own_n:
            roots.append(r)
    roots.sort(key=len, reverse=True)
    return roots


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


def remap(path, own, roots, incs):
    """path: one replayed include path (str). Returns the path in this checkout, or path unchanged when none is found."""
    if os.path.exists(path) and norm(path).startswith(norm(own)):
        return path
    n = norm(path)
    for r in roots:
        rn = norm(r) + os.sep
        if n.startswith(rn):
            # norm lower-cased the text; take the tail from the original spelling instead.
            tail = path[len(rn):]
            cand = os.path.join(own, tail)
            if os.path.exists(cand):
                return cand
            # A generated header of the other checkout's build directory: find it by its trailing components under this
            # command line's own -I directories (e.g. <build>/generated/build_fingerprint.h).
            parts = tail.replace("/", os.sep).split(os.sep)
            for i in range(len(parts)):
                sub = os.sep.join(parts[i:])
                for d in incs:
                    c = os.path.join(d, sub)
                    if os.path.exists(c) and norm(c).startswith(norm(own)):
                        return c
    return path


def main():
    own, sccache, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
    roots = roots_from_env(own)
    incs = include_dirs(cmd)
    proc = subprocess.run([sccache] + cmd, stdout=subprocess.PIPE)
    out = proc.stdout
    if PREFIX in out and roots:
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
                new = remap(path, own, roots, incs)
                if new != path:
                    eol = b"\r" if ln.endswith(b"\r") else b""
                    lines[i] = PREFIX + lead + new.encode("mbcs") + eol
        out = b"\n".join(lines)
    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())

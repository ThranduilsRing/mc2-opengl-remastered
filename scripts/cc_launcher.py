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
import deps_guard  # same directory, stdlib only

PREFIX = b"Note: including file:"


def norm(p):
    return os.path.normcase(os.path.normpath(p))


def include_dirs(args):
    dirs = []
    it = iter(args)
    for a in it:
        if a in ("/I", "-I", "/external:I", "-external:I"):
            a = next(it, "")
        elif a[:2] in ("/I", "-I"):
            a = a[2:]
        elif a[:11] in ("/external:I", "-external:I"):
            a = a[11:]
        else:
            continue
        if a:
            dirs.append(a)
    return dirs


def outside_dirs(own, incs, environ=None):
    """Normalised directories this compile searches that lie OUTSIDE the checkout (INCLUDE env + every -I not under own)."""
    own_n = norm(own) + os.sep
    env = os.environ if environ is None else environ
    cands = [d for d in env.get("INCLUDE", "").split(os.pathsep) if d] + list(incs)
    return tuple(n + os.sep for n in (norm(d) for d in cands) if not (n + os.sep).startswith(own_n))


def remap(path, own, incs, outside=None):
    """path: one replayed include path (str). Returns the path in this checkout, or path unchanged when none is found.

    A path under a directory this compile searches outside the checkout (INCLUDE env, foreign -I) is a system/SDK header and
    is returned unchanged without any search. Otherwise root-agnostic: the longest trailing part (>= 2 components) that is a
    FILE under the checkout root; this command line's own -I directories under the checkout also match a file (any length,
    for generated headers)."""
    path = path.replace("\\", "/")
    own_n = norm(own) + os.sep
    n = norm(path)
    if n.startswith(own_n):
        return path
    if outside is None:
        outside = outside_dirs(own, incs)
    if any(n.startswith(d) for d in outside):
        return path
    parts = [p for p in path.split("/") if p]
    if len(parts) > 1 and path[1:2] == ":":
        parts = parts[1:]
    own_incs = [d for d in incs if (norm(d) + os.sep).startswith(own_n)]
    for i in range(len(parts)):
        sub = os.sep.join(parts[i:])
        if len(parts) - i >= 2:
            cand = os.path.join(own, sub)
            if os.path.isfile(cand):
                return cand
        for d in own_incs:
            cand = os.path.join(d, sub)
            if os.path.isfile(cand):
                return cand
    return path


def rewrite_output(out, own, incs, outside):
    """(rewritten stdout bytes, True when a replayed include path is another checkout's and has no counterpart here)."""
    foreign = False
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
                new = remap(path, own, incs, outside)
                if deps_guard.is_foreign(new, own):   # still another checkout's path: no counterpart here
                    foreign = True
                if new != path:
                    eol = b"\r" if ln.endswith(b"\r") else b""
                    lines[i] = PREFIX + lead + new.encode("mbcs") + eol
        out = b"\n".join(lines)
    return out, foreign


def main():
    own, sccache, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
    incs = include_dirs(cmd)
    outside = outside_dirs(own, incs)
    proc = subprocess.run([sccache] + cmd, stdout=subprocess.PIPE, **console_child.child_kwargs(redirected=True))
    out, foreign = rewrite_output(proc.stdout, own, incs, outside)
    if foreign:   # fail closed: a stored entry names another checkout's header that does not exist here; recompile and overwrite it
        proc = subprocess.run([sccache] + cmd, stdout=subprocess.PIPE, env=dict(os.environ, SCCACHE_RECACHE="1"),
                              **console_child.child_kwargs(redirected=True))
        out, _ = rewrite_output(proc.stdout, own, incs, outside)
    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())

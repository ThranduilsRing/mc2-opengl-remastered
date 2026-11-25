#!/usr/bin/env python3
"""deps_guard.py -- find (and optionally delete) Ninja objects whose recorded header deps point into ANOTHER checkout.

WHY: an object whose .ninja_deps record names headers of a different checkout (A:/Games/mc2-slotNN/..., G:/mc2-wt/<x>/...) is never
dirtied by edits to THIS checkout's headers, so warm builds ship stale objects (crash at mission launch, 2026-11). The cause was
sccache replaying /showIncludes output written by another checkout (see cc_launcher.py). One scan, shared by ninja_build.py,
deploy_payload.py and cc_launcher.py. Stdlib only (cc_launcher runs under python -S -E).

usage: py -3 scripts/deps_guard.py [<tree dir>] [--delete]      (default tree: <this checkout>/build64-ninja; report only without --delete)
"""
import os
import re
import subprocess
import sys

NINJA = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"

# A path that looks like a checkout of this project: A:/Games/mc2-main, A:/Games/mc2-slotNN, G:/mc2-wt/<x>, G:/mc2-installs/...
# System/SDK/vcpkg paths (C:/Program Files, vcpkg trees, ...) never match.
_PROJECT = re.compile(r"^[a-z]:/(?:games/)?mc2[-_]", re.I)


def _norm(p):
    return p.replace("\\", "/").rstrip("/").lower()


def is_foreign(path, root):
    """True when path is a project-looking absolute path outside the checkout root."""
    n = _norm(path)
    r = _norm(root)
    if n == r or n.startswith(r + "/"):
        return False
    return bool(_PROJECT.match(n))


def parse_deps(text):
    """`ninja -t deps` text -> {object path: [dep paths]}. Records are `<obj>: #deps N, deps mtime M (VALID)` + indented deps."""
    out, cur = {}, None
    for ln in text.splitlines():
        if not ln.strip():
            cur = None
        elif ln[0] in " \t":
            if cur is not None:
                out[cur].append(ln.strip())
        elif ": #deps" in ln:
            cur = ln.split(": #deps", 1)[0]
            out[cur] = []
        else:
            cur = None
    return out


def foreign_objects(deps, root):
    """{object: first foreign dep} for every record naming a foreign path."""
    found = {}
    for obj, paths in deps.items():
        for p in paths:
            if is_foreign(p, root):
                found[obj] = p
                break
    return found


def dump_deps(tree_path):
    import console_child  # same directory; imported here so cc_launcher (which needs only is_foreign) stays light
    out = subprocess.run([NINJA, "-C", tree_path, "-t", "deps"], capture_output=True, text=True, errors="replace", timeout=300,
                         **console_child.child_kwargs(redirected=True))
    if out.returncode != 0:
        raise RuntimeError(f"ninja -t deps failed rc={out.returncode}: {out.stderr.strip()[:300]}")
    return out.stdout


def scan(tree_path, root=None):
    """{object: foreign dep} for objects that still EXIST in the tree (a record for a deleted object is dead and harmless)."""
    tree_path = tree_path.replace("\\", "/").rstrip("/")
    root = root or os.path.dirname(tree_path)
    if not os.path.isfile(f"{tree_path}/.ninja_deps"):
        return {}
    found = foreign_objects(parse_deps(dump_deps(tree_path)), root)
    return {o: p for o, p in found.items() if os.path.isfile(os.path.join(tree_path, o))}


def delete_objects(tree_path, objs):
    n = 0
    for o in objs:
        try:
            os.remove(os.path.join(tree_path, o))
            n += 1
        except OSError as exc:
            print(f"[DEPS_GUARD] could not delete {o}: {exc}", file=sys.stderr, flush=True)
    return n


def guard_before(tree_path, root=None):
    """Delete every existing object with foreign deps so Ninja rebuilds it. Returns the count deleted. Never raises on a scan fault."""
    try:
        found = scan(tree_path, root)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"[DEPS_GUARD] scan skipped: {exc}", flush=True)
        return 0
    if found:
        sample = next(iter(found.items()))
        print(f"[DEPS_GUARD] deleting {len(found)} object(s) with deps outside this checkout (e.g. {sample[0]} -> {sample[1]})", flush=True)
        delete_objects(tree_path, found)
    else:
        print("[DEPS_GUARD] 0 objects with foreign deps", flush=True)
    return len(found)


def refusal_after(tree_path, root=None):
    """Error text when any existing object still has foreign deps, else None."""
    found = scan(tree_path, root)
    if not found:
        return None
    sample = ", ".join(f"{o} -> {p}" for o, p in list(found.items())[:3])
    return (f"[DEPS_GUARD] {len(found)} object(s) in {tree_path} record header deps in another checkout ({sample}). Header edits here "
            f"would not rebuild them. Fix: delete {tree_path}/.ninja_deps (or run scripts/deps_guard.py {tree_path} --delete), then "
            "py -3 scripts/ninja_build.py")


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tree = (argv[0] if argv else os.path.join(here, "build64-ninja")).replace("\\", "/")
    found = scan(tree)
    print(f"[DEPS_GUARD] {tree}: {len(found)} existing object(s) with foreign deps")
    for o, p in list(found.items())[:10]:
        print(f"  {o} -> {p}")
    if "--delete" in sys.argv:
        print(f"[DEPS_GUARD] deleted {delete_objects(tree, found)}")
    return 1 if found and "--delete" not in sys.argv else 0


if __name__ == "__main__":
    sys.exit(main())

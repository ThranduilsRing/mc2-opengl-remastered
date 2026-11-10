#!/usr/bin/env python3
"""clean_install_byproducts.py — one-time install byproduct cleaner (dry-run by default).

Deploys still WARN that installs carry offline-cook byproducts under
data/missions/*.beauty (deploy_payload.py never deletes non-payload files, and
2585af79f only stopped new ones from being tracked). This tool lists those
byproducts and, with --apply, deletes them.

Safety invariants:
  * Only files under <install>/data/missions/*.beauty/ are candidates; nothing
    outside a .beauty dir is ever matched.
  * Only basenames matching the DIAGNOSTIC_PATTERNS of
    scripts/check-deploy-hygiene.py (imported, not copied) are candidates.
  * Basenames that `git ls-files tests/terrain/beautify` shows as tracked are
    runtime sidecars the engine loads (e.g. visual_height_16x.r32 for
    mc2_17/24) and are never candidates.
  * Symlinks/junctions are neither followed nor deleted.
  * Refuses (exit 2) when the install is or is inside the golden
    mc2-win64-v0.4c, any owner-test-* dir, A:/Games/MC2 OpenGL Remastered or
    E:/mc2-archive-2026-10.

Usage:
    py -3 scripts/clean_install_byproducts.py <install> [--apply]

Exit codes:
    0  success (dry run lists; --apply deletes)
    1  usage/IO error (not a directory, git failure)
    2  refused root
"""
import argparse
import fnmatch
import importlib.util
import os
import subprocess
import sys
from pathlib import Path, PurePosixPath

import console_child

REPO = Path(__file__).resolve().parents[1]
COOK_ROOT = "tests/terrain/beautify"

# The byproduct patterns live in check-deploy-hygiene.py; import them so the two
# tools can never drift apart.
_hygiene_spec = importlib.util.spec_from_file_location(
    "check_deploy_hygiene", Path(__file__).with_name("check-deploy-hygiene.py"))
HYGIENE = importlib.util.module_from_spec(_hygiene_spec)
_hygiene_spec.loader.exec_module(HYGIENE)

REFUSED_ABS_ROOTS = ("a:/games/mc2 opengl remastered", "e:/mc2-archive-2026-10")

REPARSE_POINT = 0x400  # FILE_ATTRIBUTE_REPARSE_POINT


def _is_link(path):
    """Symlink or Windows junction/mount point.

    Since Python 3.13 islink()/is_symlink() report S_IFLNK for symbolic links
    only, so a junction (reparse point, not S_IFLNK) slips through and
    os.walk(followlinks=False) descends into it; the raw attribute catches it.
    """
    if os.path.islink(path):
        return True
    if os.name == "nt":
        try:
            attrs = os.stat(path, follow_symlinks=False).st_file_attributes
        except (OSError, AttributeError):
            return False
        return bool(attrs & REPARSE_POINT)
    return False


def refusal_reason(install):
    """Why this install must never be cleaned, or None when it may be."""
    p = Path(install).resolve()
    for part in p.parts:
        if part.casefold() == "mc2-win64-v0.4c":
            return f"the golden install mc2-win64-v0.4c (path: {install})"
        if fnmatch.fnmatch(part.casefold(), "owner-test-*"):
            return f"an owner-test install (path: {install})"
    posix = p.as_posix().casefold()
    for root in REFUSED_ABS_ROOTS:
        if posix == root or posix.startswith(root + "/"):
            return f"the protected root {root} (path: {install})"
    return None


def tracked_sidecar_names(repo):
    """Basenames git tracks under tests/terrain/beautify: runtime sidecars the engine loads."""
    out = subprocess.run(
        ["git", "-C", str(repo), "ls-files", "--", COOK_ROOT],
        capture_output=True, text=True, timeout=60,
        **console_child.child_kwargs(redirected=True))
    if out.returncode != 0:
        raise RuntimeError(f"git ls-files {COOK_ROOT} in {repo} failed: {out.stderr.strip()}")
    return {PurePosixPath(line.strip()).name.casefold()
            for line in out.stdout.splitlines() if line.strip()}


def find_byproducts(install, tracked_names):
    """[(path, size)] for every byproduct under <install>/data/missions/*.beauty/."""
    missions = install / "data" / "missions"
    found = []
    if not missions.is_dir():
        return found
    for beauty in sorted(missions.iterdir()):
        if _is_link(beauty) or not beauty.is_dir():
            continue
        if not beauty.name.casefold().endswith(".beauty"):
            continue
        for dirpath, dirs, files in os.walk(beauty, followlinks=False):
            dirs[:] = [d for d in dirs if not _is_link(os.path.join(dirpath, d))]
            for fn in sorted(files):
                full = os.path.join(dirpath, fn)
                if _is_link(full) or not os.path.isfile(full):
                    continue
                if fn.casefold() in tracked_names:
                    continue
                if not HYGIENE.is_diagnostic(fn):
                    continue
                found.append((Path(full), os.path.getsize(full)))
    found.sort()
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="List (and with --apply delete) offline-cook byproducts under an "
                    "install's data/missions/*.beauty/ dirs.")
    ap.add_argument("install", help="install directory to clean")
    ap.add_argument("--apply", action="store_true",
                    help="delete the listed byproducts (default: dry run, deletes nothing)")
    ap.add_argument("--repo", default=str(REPO),
                    help="repo to read tracked sidecar names from (default: this script's repo)")
    args = ap.parse_args(argv)

    reason = refusal_reason(args.install)
    if reason:
        print(f"[clean-install-byproducts] REFUSED: {reason}. Nothing was touched.",
              file=sys.stderr)
        return 2

    install = Path(args.install)
    if not install.is_dir():
        print(f"[clean-install-byproducts] FAIL: not a directory: {install}", file=sys.stderr)
        return 1

    try:
        tracked = tracked_sidecar_names(Path(args.repo))
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"[clean-install-byproducts] FAIL: {exc}", file=sys.stderr)
        return 1

    byproducts = find_byproducts(install, tracked)
    total = sum(size for _, size in byproducts)
    if args.apply:
        for path, _ in byproducts:
            path.unlink()
            print(f"deleted {path.relative_to(install).as_posix()}")
        print(f"[clean-install-byproducts] deleted {len(byproducts)} byproduct(s), "
              f"{total} bytes")
    else:
        for path, size in byproducts:
            print(f"    {path.relative_to(install).as_posix()} ({size} bytes)")
        print(f"[clean-install-byproducts] dry run: {len(byproducts)} byproduct(s), "
              f"{total} bytes under {install}")
        if byproducts:
            print("  Re-run with --apply to delete them.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

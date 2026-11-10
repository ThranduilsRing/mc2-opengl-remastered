#!/usr/bin/env python3
"""Lane-install data parity with the nifty install (v0.4).

A lane install built from E:/mc2-installs/_lean-seed carries the deploy payload and the stock archives, but the seed
used to leave out the pre-baked data the game reads from v0.4: the terrain transition cache (data/textures/*.txm, *.ktx2,
*.lst, *.burnin.jpg), the loose terrain overlay/mask art and the stock option files. Without them a lane regenerates the
transition textures at load (127 s on mc2_01, and the concrete/shore areas then differ from v0.4), so every lane A/B and
smoke look differed from v0.4 for data reasons, not code reasons.

The seed is "v0.4 data minus the exe": PARITY below names that data. `--refresh-seed` copies it from v0.4 (read only) into
the seed; `make-isolated-deploy.py` admits it into every lane install and then fails the install if any of it differs
from v0.4 (`require_parity`). Not parity (deploy-managed or per-install): the exe/dll/shader payload, mods, cache/, the
runtime-written dirs.

  py -3 scripts/seed_parity.py --check E:/mc2-installs/<install>
  py -3 scripts/seed_parity.py --refresh-seed E:/mc2-installs/_lean-seed
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PREFIX = "[SEED_PARITY]"
DEFAULT_REF = Path(os.environ.get("MC2_SEED_PARITY_REF", "A:/Games/mc2-opengl/mc2-win64-v0.4"))

ROOT_FILES = ("options.cfg", "system.cfg", "prefs.cfg", "minprefs.cfg", "orgprefs.cfg")
# (prefix, excluded subdirectories under it). Backups and the random-map picker art are never read by a mission.
TREES = (
    ("data/textures/", ("_txm_cache_bak/", "txm_backup/", "random_maps/")),
    ("data/art/mechicons/", ()),
    ("saved_bookmarks/", ()),
)
SKIP_SUFFIXES = (".bak", ".orig", ".orig_normal", ".tmp", ".log")


def is_parity(rel: str) -> bool:
    """True when `rel` (install-relative, any case/slashes) is data a lane install must carry exactly as v0.4 does."""
    low = rel.replace("\\", "/").lstrip("/").casefold()
    if low in ROOT_FILES:
        return True
    if low.endswith(SKIP_SUFFIXES):
        return False
    for prefix, excluded in TREES:
        if low.startswith(prefix):
            return not any(low.startswith(prefix + e) for e in excluded)
    return False


def reference_files(ref: Path) -> dict[str, int]:
    """{relpath: size} of every parity file in the reference install."""
    out: dict[str, int] = {}
    for name in ROOT_FILES:
        p = ref / name
        if p.is_file():
            out[name] = p.stat().st_size
    for prefix, _ in TREES:
        base = ref / prefix.rstrip("/")
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                p = Path(dirpath) / fn
                rel = p.relative_to(ref).as_posix()
                if is_parity(rel):
                    out[rel] = p.stat().st_size
    return out


def _sha(path: Path) -> str:
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def differing(install: Path, ref: Path, workers: int = 6) -> list[tuple[str, str]]:
    """(kind, relpath) of parity files missing from `install` or different from the reference (size, then sha256)."""
    problems: list[tuple[str, str]] = []
    need_hash = []
    for rel, size in reference_files(ref).items():
        try:
            got = (install / rel).stat().st_size
        except OSError:
            problems.append(("missing", rel))
            continue
        if got != size:
            problems.append(("size", rel))
        else:
            need_hash.append(rel)

    def same(rel: str) -> bool:
        return _sha(install / rel) == _sha(ref / rel)

    with ThreadPoolExecutor(workers) as pool:
        problems += [("content", rel) for rel, ok in zip(need_hash, pool.map(same, need_hash)) if not ok]
    return sorted(problems, key=lambda kr: kr[1])


def refresh_seed(seed: Path, ref: Path, dry_run: bool = False) -> int:
    """Copy every parity file that is missing or different from `ref` into `seed`. Never writes `ref`."""
    seed, ref = seed.resolve(), ref.resolve()
    if seed == ref or seed in ref.parents or ref in seed.parents:
        raise SystemExit(f"{PREFIX} FAIL: seed and reference must be different trees ({seed} / {ref})")
    todo = [rel for _kind, rel in differing(seed, ref)]
    for rel in todo:
        if not dry_run:
            dst = seed / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.exists():
                dst.unlink()          # a hardlinked seed file must not be rewritten through its inode
            shutil.copy2(ref / rel, dst)
    print(f"{PREFIX} {'would copy' if dry_run else 'copied'} {len(todo)} parity file(s) {ref} -> {seed}")
    return len(todo)


def require_parity(install: Path, ref: Path | None = None) -> None:
    """Fail (SystemExit 3) when a freshly built lane install's parity data differs from v0.4's."""
    ref = Path(ref) if ref else DEFAULT_REF
    if not ref.is_dir():
        print(f"{PREFIX} SKIPPED: reference {ref} not found")
        return
    problems = [f"{kind} {rel}" for kind, rel in differing(Path(install), ref)]
    if problems:
        shown = "\n  ".join(problems[:20])
        raise SystemExit(
            f"{PREFIX} FAIL: {len(problems)} data file(s) of {install} differ from {ref}:\n  {shown}"
            + ("\n  ..." if len(problems) > 20 else "")
            + f"\n  fix: py -3 scripts/seed_parity.py --refresh-seed E:/mc2-installs/_lean-seed, then rebuild the install")
    print(f"{PREFIX} OK: {install} matches {ref} for the shared data set")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", default=str(DEFAULT_REF), help="reference install (read only)")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", metavar="INSTALL")
    group.add_argument("--refresh-seed", metavar="SEED")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    ref = Path(args.ref)
    if args.check:
        require_parity(Path(args.check), ref)
    else:
        refresh_seed(Path(args.refresh_seed), ref, args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())

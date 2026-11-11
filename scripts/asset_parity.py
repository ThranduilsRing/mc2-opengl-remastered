#!/usr/bin/env python3
"""Lane installs must carry the v0.4 install's assets (owner 2026-11-11).

A lane install deployed with --skip-kit (E:/mc2-installs/quiet-A) was missing 800 MB / 16,254 v0.4 data
files and had 2,354 textures of a different size; its captures showed stock concrete. Every lane deploy
now compares data/, assets/ and the root *.fst archives with the v0.4 install by path and size, plus
sha256 for a sampled subset, SYNCS missing or different files from v0.4 (read only), and deletes
unowned extras after reporting dry-run counts. With
--no-asset-sync the deploy fails instead, naming the counts.

The one-line `[ASSET_PARITY] OK|SYNCED|FAIL|SKIPPED ...` verdict is printed by deploy_payload and
run_smoke; the capture tools refuse a drifted install unless --allow-asset-drift is given.

  py -3 scripts/asset_parity.py --check E:/mc2-installs/<lane>    # counts only, no copy
  py -3 scripts/asset_parity.py --sync  E:/mc2-installs/<lane>
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

PREFIX = "[ASSET_PARITY]"
TREES = ("data", "assets")
ROOT_SUFFIX = ".fst"
# Backups of the texture-transition cache; never read by a mission (seed_parity excludes them too).
EXCLUDED = ("data/textures/_txm_cache_bak/", "data/textures/txm_backup/")
SAMPLE_FILES = 256
SAMPLE_BYTES = 32 << 20
# Per-install receipts written by the kit reconcile; not asset content.
SKIP_SUFFIXES = (".cookstamp",)
# v0.4 is the read-only reference; the golden install is never a parity target either.
GOLDEN_NAMES = {"mc2-win64-v0.4c", "mc2-win64-0.4c"}


def default_ref() -> Path:
    """The v0.4 install asset parity is measured against (override: MC2_ASSET_PARITY_REF)."""
    override = os.environ.get("MC2_ASSET_PARITY_REF")
    if override:
        return Path(override)
    return Path("A:/Games/mc2-opengl/mc2-win64-v0.4")  # drive-path: ok the v0.4 reference install lives on A: on every Windows host; the box passes MC2_ASSET_PARITY_REF


def _rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _norm(rel: str) -> str:
    return rel.replace("\\", "/").lstrip("/").casefold()


def is_excluded(rel: str) -> bool:
    low = _norm(rel)
    return any(low.startswith(prefix) for prefix in EXCLUDED)


def is_exempt(install: Path | str, ref: Path | str | None = None) -> bool:
    """True for the reference install itself and the golden: they are the source of truth, never a target."""
    install = Path(install)
    ref = Path(ref) if ref is not None else default_ref()
    if os.path.normcase(os.path.abspath(install)) == os.path.normcase(os.path.abspath(ref)):
        return True
    return install.name.casefold() in {name.casefold() for name in GOLDEN_NAMES}

def reference_index(ref: Path) -> dict[str, tuple[str, int]]:
    """{casefolded relpath: (relpath, size)} of every parity file in the reference install."""
    ref = Path(ref)
    out: dict[str, tuple[str, int]] = {}
    for tree in TREES:
        base = ref / tree
        if not base.is_dir():
            continue
        for dirpath, dirs, files in os.walk(base):
            dirs[:] = sorted(d for d in dirs if not is_excluded(_rel(Path(dirpath) / d, ref) + "/"))
            for name in files:
                path = Path(dirpath) / name
                rel = _rel(path, ref)
                if is_excluded(rel):
                    continue
                try:
                    out[_norm(rel)] = (rel, path.stat().st_size)
                except OSError:
                    continue
    if ref.is_dir():
        for path in sorted(ref.glob("*" + ROOT_SUFFIX)):
            if path.is_file():
                out[_norm(path.name)] = (path.name, path.stat().st_size)
    return out


@dataclass
class Result:
    """What a parity check found; the lists hold install-relative paths."""
    missing: list[str] = field(default_factory=list)
    size: list[str] = field(default_factory=list)
    content: list[str] = field(default_factory=list)
    extra: list[str] = field(default_factory=list)
    sampled: int = 0
    total: int = 0
    synced: int = 0
    synced_bytes: int = 0
    deleted: int = 0

    @property
    def ok(self) -> bool:
        return not (self.missing or self.size or self.content or self.extra)


def sample_rels(rels: list[str], limit: int = SAMPLE_FILES) -> list[str]:
    """A deterministic spread over the sorted equal-size files (never a prefix of one directory)."""
    ordered = sorted(rels)
    if len(ordered) <= limit:
        return ordered
    stride = len(ordered) / limit
    return [ordered[int(i * stride)] for i in range(limit)]


def _sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compare(install: Path | str, ref: Path | str, sample: int = SAMPLE_FILES,
            sample_bytes: int = SAMPLE_BYTES, skip: "set[str] | None" = None) -> Result:
    """Missing, size-different and (sampled) content-different parity files of `install` against `ref`.

    `skip` holds casefolded relpaths this deploy itself owns (the payload): those are shipped on purpose and
    must never be replaced by the reference's older copy."""
    install, ref = Path(install), Path(ref)
    skip = skip or set()
    index = {key: value for key, value in reference_index(ref).items()
             if key not in skip and not value[0].casefold().endswith(SKIP_SUFFIXES)}
    result = Result(total=len(index))
    result.extra = sorted(rel for key, (rel, _size) in reference_index(install).items()
                          if key not in index and key not in skip
                          and not rel.casefold().endswith(SKIP_SUFFIXES))
    equal: list[str] = []
    for rel, size in index.values():
        try:
            got = (install / rel).stat().st_size
        except OSError:
            result.missing.append(rel)
            continue
        if got != size:
            result.size.append(rel)
        else:
            equal.append(rel)
    budget = sample_bytes
    for rel in sample_rels(equal, max(0, sample)):
        if budget <= 0:
            break
        budget -= index[_norm(rel)][1]
        try:
            same = _sha(install / rel) == _sha(ref / rel)
        except OSError:
            same = False
        if not same:
            result.content.append(rel)
        result.sampled += 1
    result.missing.sort()
    result.size.sort()
    result.content.sort()
    return result


def verdict_line(result: Result, ref: Path | str, status: str | None = None) -> str:
    """The one line run_smoke and the capture tools read."""
    state = status or ("SYNCED" if result.ok and (result.synced or result.deleted) else "OK" if result.ok else "FAIL")
    line = (f"{PREFIX} {state} ref={Path(ref).as_posix()} files={result.total} "
            f"missing={len(result.missing)} size={len(result.size)} content={len(result.content)} "
            f"extra={len(result.extra)} sampled={result.sampled}")
    if result.synced:
        line += f" synced={result.synced} bytes={result.synced_bytes}"
    if result.deleted:
        line += f" deleted={result.deleted}"
    return line

def _copy(src: Path, dst: Path) -> None:
    """Copy one file atomically: a shared hardlink in dst is replaced, never written through."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + f".asset-parity-{os.getpid()}")
    try:
        shutil.copy2(src, tmp)
        if dst.exists():
            dst.chmod(dst.stat().st_mode | stat.S_IWRITE)
        os.replace(tmp, dst)
    finally:
        if tmp.exists():
            tmp.chmod(tmp.stat().st_mode | stat.S_IWRITE)
        tmp.unlink(missing_ok=True)


def sync(install: Path | str, ref: Path | str, result: Result, jobs: int = 4) -> Result:
    """Copy the missing/different files from `ref` (read only) into `install`; returns the same Result."""
    install, ref = Path(install), Path(ref)
    index = reference_index(ref)
    sizes = {rel: size for rel, size in index.values()}
    todo = result.missing + result.size + result.content
    # Print the complete mutation count before the first copy or deletion.
    print(verdict_line(result, ref, status="DRY_RUN"), flush=True)

    def one(rel: str) -> int:
        _copy(ref / rel, install / rel)
        return sizes[rel] if rel in sizes else (install / rel).stat().st_size

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        written = list(pool.map(one, todo))
    result.synced, result.synced_bytes = len(written), sum(written)
    for rel in result.extra:
        path = install / rel
        path.chmod(path.stat().st_mode | stat.S_IWRITE)
        path.unlink()
        result.deleted += 1
        print(f"{PREFIX} deleted {rel}", flush=True)
    result.missing, result.size, result.content = [], [], []
    result.extra = []
    return result


def enforce(install: Path | str, ref: Path | str | None = None, sync_missing: bool = True,
            jobs: int = 4, skip: "set[str] | None" = None) -> Result | None:
    """Check (and unless sync_missing is False, sync) one lane install; None when exempt or no reference."""
    ref = Path(ref) if ref is not None else default_ref()
    if is_exempt(install, ref) or not ref.is_dir():
        return None
    result = compare(install, ref, skip=skip)
    if sync_missing and not result.ok:
        result = sync(install, ref, result, jobs)
    return result


def capture_drift(install: Path | str, ref: Path | str | None = None) -> Result | None:
    """The drift a capture must refuse (None when exempt or when the reference is not on this host)."""
    ref = Path(ref) if ref is not None else default_ref()
    if is_exempt(install, ref) or not ref.is_dir():
        return None
    return compare(install, ref)


def jobs() -> int:
    """A:/Games/mc2-ops/JOBS, the one parallelism source; 4 when the file is absent (tests, the box)."""
    try:
        return max(1, int(Path("A:/Games/mc2-ops/JOBS").read_text().strip()))  # drive-path: ok the ops JOBS file is an A: path everywhere; 4 when absent
    except (OSError, ValueError):
        return 4


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("install", help="lane install to check")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="report counts only; never copies")
    group.add_argument("--sync", action="store_true", help="copy missing/different files and delete unowned extras")
    ap.add_argument("--ref", default=None, help="reference install (default: %s)" % default_ref())
    args = ap.parse_args(argv)
    ref = Path(args.ref) if args.ref else default_ref()
    install = Path(args.install)
    if is_exempt(install, ref):
        print(f"{PREFIX} SKIPPED {install.as_posix()} is the reference/golden install")
        return 0
    if not ref.is_dir():
        print(f"{PREFIX} SKIPPED reference not found: {ref.as_posix()}")
        return 0
    result = compare(install, ref)
    if args.sync and not result.ok:
        result = sync(install, ref, result, jobs())
    print(verdict_line(result, ref))
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())

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
import csv
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
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
# Paths named per category in a FAIL detail (the one-line verdict stays parseable).
DETAIL_LIMIT = 8
# Per-install receipts written by the kit reconcile; not asset content.
SKIP_SUFFIXES = (".cookstamp",)
# v0.4 is the read-only reference; the golden install is never a parity target either.
GOLDEN_NAMES = {"mc2-win64-v0.4c", "mc2-win64-0.4c"}
INTENTIONAL = "intentional-asset-changes.json"
AUXILIARY = {"crashpad_handler.exe", "mc2res_64.dll"}


def intentional_changes(commit: str | None, root: Path | None = None) -> dict[str, str]:
    """Read the candidate's tracked {install-relative path: sha256} list at its deployed commit.

    Each lane commits intentional-asset-changes.json with its candidate. No working-copy or
    environment allowlist is trusted. Baseline arms use only their own commit's declarations.
    """
    if not commit or not re.fullmatch(r"[0-9a-f]{40}", commit):
        return {}
    import console_child
    root = root or Path(__file__).resolve().parents[1]
    proc = subprocess.run(["git", "-C", str(root), "ls-tree", "--name-only", commit, "--", INTENTIONAL],
                          capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if proc.returncode:
        # An unavailable commit grants no exceptions; ancestry/identity gates own commit availability.
        return {}
    if not proc.stdout.strip():
        return {}
    proc = subprocess.run(["git", "-C", str(root), "show", f"{commit}:{INTENTIONAL}"],
                          capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if proc.returncode:
        raise ValueError(f"cannot read {INTENTIONAL} at {commit}")
    entries = json.loads(proc.stdout)
    if not isinstance(entries, dict):
        raise ValueError(f"{INTENTIONAL}: expected path -> sha256 object")
    for rel, sha in entries.items():
        if (not isinstance(rel, str) or rel != _norm(rel) or ".." in rel.split("/")
                or ":" in rel or not rel or rel.startswith("/")
                or not (rel.startswith(("data/", "assets/")) or rel in AUXILIARY
                        or ("/" not in rel and rel.endswith(ROOT_SUFFIX)))
                or not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha)):
            raise ValueError(f"{INTENTIONAL}: invalid path/hash: {rel!r}")
    return entries


def manifest_assets(install: Path | str) -> tuple[dict[str, str], str | None]:
    """The deploy manifest is the single inventory of payload-owned asset paths."""
    path = Path(install) / ".deployed_manifest.csv"
    if not path.exists():
        return {}, None
    with path.open(newline="", encoding="utf-8") as stream:
        rows = csv.reader(stream)
        if next(rows, [])[:2] != ["manifest_version", "v1"]:
            raise ValueError(f"invalid payload manifest: {path}")
        entries = list(csv.DictReader(stream))
    commits = {row["src_commit"] for row in entries if row["relpath"] in {"mc2.exe", "mc2", "mc2.elf"}}
    if len(commits) != 1:
        raise ValueError(f"missing/ambiguous executable commit: {path}")
    return {row["relpath"]: row["sha256"] for row in entries}, commits.pop()


def owned_paths(install: Path | str, payload: dict[str, str] | None = None,
                declared: dict[str, str] | None = None) -> set[str]:
    """Only exact manifest/declaration bytes are owned, in deploy and capture alike."""
    install = Path(install)
    if payload is None:
        payload, commit = manifest_assets(install)
        declared = intentional_changes(commit) if declared is None else declared
    expected = dict(payload)
    expected.update(declared or {})
    bad = [f"{rel} ({_why(install / rel, sha)})" for rel, sha in (declared or {}).items()
           if not (install / rel).is_file() or _sha(install / rel) != sha]
    if bad:
        raise ValueError(_bad_message("INTENTIONAL ASSET hash mismatch", bad))
    for rel, sha in (declared or {}).items():
        print(f"{PREFIX} INTENTIONAL ASSET ACCEPTED {rel} sha256={sha}", flush=True)
    owned = set()
    bad = []
    for rel, sha in expected.items():
        low = _norm(rel)
        if not (low.startswith(("data/", "assets/")) or ("/" not in low and low.endswith(ROOT_SUFFIX))):
            continue
        path = (install / rel).resolve()
        if not path.is_relative_to(install.resolve()) or not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError(f"invalid owned payload path/hash: {rel}")
        if not path.is_file() or _sha(path) != sha:
            bad.append(f"{rel} ({_why(path, sha)})")
            continue
        owned.add(low)
    if bad:
        raise ValueError(_bad_message("owned payload missing or hash mismatch", bad))
    return owned


def _why(path: Path, sha: str) -> str:
    """Short reason a pinned file fails: absent, or the actual vs expected sha prefix."""
    if not path.is_file():
        return f"absent, want sha256={sha[:12]}"
    return f"sha256={_sha(path)[:12]}, want {sha[:12]}"


def _bad_message(head: str, bad: list[str], limit: int = DETAIL_LIMIT) -> str:
    """One error naming EVERY failing pinned path (first `limit`), so one run shows the whole census."""
    more = f"; +{len(bad) - limit} more" if len(bad) > limit else ""
    return f"{head}: {len(bad)} file(s): " + "; ".join(bad[:limit]) + more


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
            sample_bytes: int = SAMPLE_BYTES, skip: "set[str] | None" = None,
            payload: dict[str, str] | None = None, declared: dict[str, str] | None = None) -> Result:
    """Missing, size-different and (sampled) content-different parity files of `install` against `ref`.

    `skip` holds casefolded relpaths this deploy itself owns (the payload): those are shipped on purpose and
    must never be replaced by the reference's older copy."""
    install, ref = Path(install), Path(ref)
    skip = owned_paths(install, payload, declared) if skip is None else {_norm(p) for p in skip}
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

def detail_lines(result: Result, install: Path | str, ref: Path | str, limit: int = DETAIL_LIMIT) -> list[str]:
    """Actionable lines under a FAIL verdict: first `limit` paths per category and the likely cause.

    A reference file newer than the install's manifest means v0.4 changed after this deploy (a merge-gate
    deploy or a v0.4 run wrote it): redeploying syncs it. An extra is a file v0.4 and the payload do not
    list (earlier deploy into a reused install, or a runtime cache): --sync / redeploy prunes it."""
    install, ref = Path(install), Path(ref)
    lines = []
    for name, rels in (("missing", result.missing), ("size", result.size),
                       ("content", result.content), ("extra", result.extra)):
        if rels:
            more = f" (+{len(rels) - limit} more)" if len(rels) > limit else ""
            lines.append(f"{PREFIX}   {name}: " + ", ".join(rels[:limit]) + more)
    try:
        deployed = (install / ".deployed_manifest.csv").stat().st_mtime
        newer = [rel for rel in result.missing + result.size + result.content
                 if (ref / rel).is_file() and (ref / rel).stat().st_mtime > deployed]
    except OSError:
        newer = []
    if newer:
        lines.append(f"{PREFIX}   cause: {len(newer)} of the missing/size/content files are newer in {ref.as_posix()} "
                     "than this install's deploy (the reference moved); redeploy or run asset_parity.py --sync")
    elif result.extra and not (result.missing or result.size or result.content):
        lines.append(f"{PREFIX}   cause: extras are not in {ref.as_posix()} or the payload manifest; "
                     "redeploy or asset_parity.py --sync <install> deletes them")
    return lines


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
            jobs: int = 4, skip: "set[str] | None" = None,
            payload: dict[str, str] | None = None, declared: dict[str, str] | None = None) -> Result | None:
    """Check (and unless sync_missing is False, sync) one lane install; None when exempt or no reference."""
    ref = Path(ref) if ref is not None else default_ref()
    if is_exempt(install, ref) or not ref.is_dir():
        return None
    repaired = []
    # Declarations pin published cooks that a lean seed or a host without cook sources may lack.
    if sync_missing:
        if declared is None:
            _, commit = manifest_assets(install)
            declared = intentional_changes(commit)
        for rel, expected in (declared or {}).items():
            dst = Path(install) / rel
            if not dst.is_file() or _sha(dst) != expected:
                source = ref / rel
                if not source.is_file() or _sha(source) != expected:
                    raise ValueError(f"INTENTIONAL ASSET reference hash mismatch: {rel}")
                if not dst.resolve().is_relative_to(Path(install).resolve()):
                    raise ValueError(f"invalid owned payload path/hash: {rel}")
                _copy(source, dst)
                repaired.append(rel)
    result = compare(install, ref, skip=skip, payload=payload, declared=declared)
    if sync_missing and not result.ok:
        result = sync(install, ref, result, jobs)
    result.synced += len(repaired)
    result.synced_bytes += sum((Path(install) / rel).stat().st_size for rel in repaired)
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
    result = enforce(install, ref, sync_missing=args.sync, jobs=jobs())
    print(verdict_line(result, ref))
    for line in detail_lines(result, install, ref):
        print(line)
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())

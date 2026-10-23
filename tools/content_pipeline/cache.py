"""Content-addressed cook cache shared by every offline cook step.

WHAT: one file per (kind, key) named <prefix><key as 16 hex digits><ext>, the same
layout and publication rules as the engine's runtime cook cache
(GameOS/gameos/cook_cache.{h,cpp}, used by the shadow and terrain-surface cooks):
write to a unique temp file in the same directory, then atomically rename over
the entry, so a crash leaves at most a .tmp (swept after an hour), never a
partial entry; after each write only the newest max_files entries of the kind
are kept.
WHY: the key is derived from the RESOLVED CONTENT BYTES of every input (after
mod VFS resolution) plus the cook contract version and parameters, never from
file names. Renaming a source, or two mods shipping the same name with
different pixels, can therefore never hit a stale entry.
"""
from __future__ import annotations

import hashlib
import json
import os
import time
from dataclasses import dataclass
from pathlib import Path

DEFAULT_DIR = Path(os.environ.get("MC2_CONTENT_CACHE", "G:/mc2-assets/cache/content-pipeline"))


@dataclass(frozen=True)
class CacheKind:
    prefix: str
    ext: str
    max_files: int = 4096


def cache_key(contract: str, params: dict, inputs: list[bytes | str]) -> int:
    """64-bit key over the contract id, canonical params and each input's content hash.

    inputs are raw bytes or already-computed sha256 hex digests of resolved bytes.
    0 is reserved (the engine treats key 0 as "no entry"), so it is remapped.
    """
    h = hashlib.sha256()
    h.update(contract.encode("utf-8") + b"\0")
    h.update(json.dumps(params, sort_keys=True, separators=(",", ":")).encode("utf-8") + b"\0")
    for item in inputs:
        digest = item if isinstance(item, str) else hashlib.sha256(item).hexdigest()
        if len(digest) != 64:
            raise ValueError("inputs must be bytes or sha256 hex digests")
        h.update(bytes.fromhex(digest))
    key = int.from_bytes(h.digest()[:8], "little")
    return key or 1


def entry_path(directory, kind: CacheKind, key: int) -> Path:
    return Path(directory) / f"{kind.prefix}{key:016x}{kind.ext}"


def read_entry(directory, kind: CacheKind, key: int) -> bytes | None:
    path = entry_path(directory, kind, key)
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if not data:
        return None
    try:
        os.utime(path)   # LRU: a hit counts as use
    except OSError:
        pass
    return data


def write_entry(directory, kind: CacheKind, key: int, data: bytes) -> Path:
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = entry_path(directory, kind, key)
    tmp = path.with_name(f"{path.name}.tmp{os.getpid()}-{time.monotonic_ns()}")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)   # atomic publication
    sweep(directory, kind, keep=path)
    return path


def sweep(directory, kind: CacheKind, keep=None):
    entries = []
    now = time.time()
    for p in Path(directory).glob(kind.prefix + "*"):
        try:
            st = p.stat()
        except OSError:
            continue
        if p.suffix == kind.ext:
            entries.append((st.st_mtime, p))
        elif ".tmp" in p.name and now - st.st_mtime > 3600:
            p.unlink(missing_ok=True)
    entries.sort(reverse=True)
    for _, p in entries[kind.max_files:]:
        if p != keep:
            p.unlink(missing_ok=True)


def cached(directory, kind: CacheKind, key: int, produce) -> bytes:
    """Return the entry for key, producing and publishing it on a miss."""
    hit = read_entry(directory, kind, key)
    if hit is not None:
        return hit
    data = produce()
    write_entry(directory, kind, key, data)
    return data

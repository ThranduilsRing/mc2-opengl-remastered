# scripts/smoke_lib/preflight_cache.py
"""SMOKE-LATENCY-WINS-1 win #4: amortize per-run harness preflights.

run_smoke.py shells out to three advisory child processes before every
mission batch: check-deploy-coherence.py (re-hashes every manifest file),
check_shader_bom.py (re-reads every shader source), and
deploy_payload.staleness_report (re-hashes every manifest file again). None
of these can change results between two `run_smoke` invocations unless the
deployed tree or the worktree HEAD changed, so a cheap fingerprint lets a
second back-to-back invocation skip all three (~2-4s/run).

FINGERPRINT (deliberately cheap -- no hashing):
  - HEAD sha of the worktree running run_smoke.py (git rev-parse HEAD)
  - deploy dir's manifest file (.deployed_manifest.csv or
    .deploy-manifest.json), mtime + size (NOT its contents)
  - shaders/ directory signature: file count + max mtime across
    shaders/**/*.{vert,frag,geom,comp,hglsl} (BOM check scope)

If any of these differ from the cached entry, the cache misses and the
caller re-runs the real checks. This is intentionally coarse: it trades a
small false-negative window (e.g. a shader edited with its mtime clock
skewed backward) for zero reimplemented hashing. All three checks are
advisory-only in run_smoke.py already (never change the smoke verdict), so
a stale cache entry has no correctness impact on gates -- worst case is a
missed WARN line, which is why this is opt-in via a flag rather than
silently always-on for a first pass.

Cache file: <ARTIFACT_ROOT>/.preflight_cache.json (sits next to smoke.lock,
not inside a per-timestamp run dir, so it persists across runs).
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Optional

SHADER_GLOBS = ["*.vert", "*.frag", "*.geom", "*.comp", "*.hglsl"]
CACHE_VERSION = 1


def _git_head(worktree: Path) -> Optional[str]:
    try:
        out = subprocess.run(
            ["git", "-C", str(worktree), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else None
    except Exception:
        return None


def _manifest_fingerprint(deploy_dir: Path) -> Optional[list]:
    """[name, mtime, size] for whichever manifest file is present (prefers the
    CSV manifest, matching check-deploy-coherence.py's _newest_manifest). None
    if neither exists -- caller must not cache in that case (nothing to key
    against, so treat as always-miss)."""
    candidates = [
        deploy_dir / ".deployed_manifest.csv",
        deploy_dir / ".deploy-manifest.json",
    ]
    present = [p for p in candidates if p.is_file()]
    if not present:
        return None
    newest = max(present, key=lambda p: p.stat().st_mtime)
    st = newest.stat()
    return [newest.name, st.st_mtime, st.st_size]


def _shaders_fingerprint(worktree: Path) -> list:
    """[file_count, max_mtime] across the shader globs BOM-checks. Cheap:
    stats every file (no reads) once per invocation -- still far cheaper than
    the O(shader-file-bytes) BOM scan it stands in for."""
    shaders_dir = worktree / "shaders"
    if not shaders_dir.is_dir():
        return [0, 0.0]
    count = 0
    max_mtime = 0.0
    for glob in SHADER_GLOBS:
        for p in shaders_dir.rglob(glob):
            try:
                st = p.stat()
            except OSError:
                continue
            count += 1
            if st.st_mtime > max_mtime:
                max_mtime = st.st_mtime
    return [count, max_mtime]


def compute_fingerprint(deploy_dir: Path, worktree: Path) -> Optional[dict]:
    """Build the full cache key. Returns None if a manifest is absent (no
    stable key to cache against -- caller should always miss/skip caching)."""
    head = _git_head(worktree)
    manifest_fp = _manifest_fingerprint(deploy_dir)
    if manifest_fp is None or head is None:
        return None
    return {
        "version": CACHE_VERSION,
        "head": head,
        "deploy_dir": str(deploy_dir.resolve()).replace("\\", "/"),
        "manifest": manifest_fp,
        "shaders": _shaders_fingerprint(worktree),
    }


def load_cache(cache_path: Path) -> dict:
    try:
        return json.loads(cache_path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {}


def save_cache(cache_path: Path, fp: dict, results: dict) -> None:
    """Atomic write (tmp + replace) so a concurrent second smoke process
    never observes a half-written cache file."""
    entry = {"fingerprint": fp, "results": results}
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache_path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(entry, indent=2), encoding="utf-8")
    import os
    os.replace(str(tmp), str(cache_path))


def check_cache_hit(cache_path: Path, fp: Optional[dict]) -> Optional[dict]:
    """Return the cached `results` dict if the on-disk cache's fingerprint
    matches `fp` exactly; None on any miss (missing file, corrupt json,
    fingerprint mismatch, or fp is None meaning "uncacheable this run")."""
    if fp is None:
        return None
    cached = load_cache(cache_path)
    if not cached:
        return None
    if cached.get("fingerprint") == fp:
        return cached.get("results")
    return None

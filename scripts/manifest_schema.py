#!/usr/bin/env python3
"""S12 -- Unified artifact manifest schema (identity block + report summary).

One shape, one identity block, one report summary -- so every MC2 tool that
emits an artifact manifest (run manifest, deploy manifest, package.json,
install receipt, cook.json, release-zip install report, visual-capture
sidecar) can carry the SAME machine-readable identity and be joined on the
same fields.

This module is the single source for that shape. It is stdlib-only and has no
package dependencies so both `scripts/` and `tools/` emitters can import it
(mirrors `scripts/telemetry_lift.py` placement; consumers either run from the
repo with `scripts/` on sys.path or insert it, as cockpit.py already does).

Roadmap: docs/superpowers/strategy/superpowers-execution-roadmap.md C11
("terminology drift: manifest") + the S11/S12 acceleration goal. This does NOT
rewrite the five existing species -- it gives them a common embeddable block
they adopt additively (old keys stay; consumers keep working).

Canonical field names (the user-facing contract -- every script reads these):

    identity.exe.sha256       <- exe_sha
    identity.git.commit       <- git_commit (full sha; .commit_short + .describe also present)
    identity.deploy_target    <- deploy_target
    identity.env_delta        <- env_delta (MC2_* deltas vs a clean env)
    identity.zip_set          <- zip_set (fst/pak set for release/package species)
    report.verdict            <- verdict
    report.missions           <- mission(s)
    report.artifact_dir       <- artifact_dir

A manifest conforms to "mc2-manifest/1" when it carries an `identity` block
that `validate_identity()` accepts. `report` is optional (only run/release/
visual species have a verdict).
"""
from __future__ import annotations

import datetime
import hashlib
import os
import platform
import subprocess
from pathlib import Path
from typing import Any, Iterable, Mapping

SCHEMA = "mc2-manifest/1"

# MC2_* env vars are the render-affecting gate surface. env_delta records the
# subset actually set in the captured environment (value as the process saw
# it). A caller may pass an explicit key allow-list; by default every MC2_*
# present in the supplied env is recorded.
_DEFAULT_ENV_PREFIX = "MC2_"


# --------------------------------------------------------------------------- #
# Primitive helpers (no raising -- a manifest must never fail to write because
# an enrichment lookup failed; missing data is recorded as null).
# --------------------------------------------------------------------------- #

def _utc_now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )


def file_sha256(path: str | os.PathLike) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 16), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return None


def _git(repo_root: str | os.PathLike, *args: str) -> str | None:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(repo_root), *args],
            stderr=subprocess.DEVNULL,
        )
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return None


def git_block(repo_root: str | os.PathLike | None) -> dict[str, Any]:
    """{commit, commit_short, branch, dirty, describe} -- any field may be null.

    Unifies the three prior namings: cockpit `git_head` (short),
    write-deploy-manifest `git_head`(full)+`git_branch`+`git_dirty`.
    """
    if repo_root is None:
        return {"commit": None, "commit_short": None, "branch": None,
                "dirty": None, "describe": None}
    commit = _git(repo_root, "rev-parse", "HEAD")
    # FINGERPRINT-CONTRADICTION FIX (VK-NOINTEROP-STRICT-REVIEW, 2026-07-10):
    # use `--porcelain -uno` (untracked-files=no) so `dirty` means TRACKED
    # modifications only -- identical to the engine build stamp
    # (cmake/GenerateBuildFingerprint.cmake:37 `git status --porcelain -uno`).
    # Plain `--porcelain` counted the worktree's untracked scratch (.claude/, run/,
    # test artifacts) as dirty, so the manifest reported dirty:true for the SAME
    # commit the engine build stamp reported dirty:false -- the fingerprint
    # contradiction the review flagged. Provenance = "was the built code from clean
    # tracked source", which untracked files never affect. `describe --dirty` is
    # already tracked-only, so all three (build stamp / manifest.dirty / describe)
    # now agree.
    porcelain = _git(repo_root, "status", "--porcelain", "-uno")
    return {
        "commit": commit,
        "commit_short": commit[:12] if commit else None,
        "branch": _git(repo_root, "rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": (bool(porcelain) if porcelain is not None else None),
        "describe": _git(repo_root, "describe", "--always", "--dirty"),
    }


def exe_block(exe_path: str | os.PathLike | None) -> dict[str, Any] | None:
    """{path, sha256, mtime_iso, size_bytes} -- full sha256 (NOT a prefix).

    Unifies cockpit `exe.sha256_prefix` (16ch) and deploy `files[].sha256`
    (full) onto one full-hash block. Returns None if exe_path is None.
    """
    if exe_path is None:
        return None
    p = Path(exe_path)
    try:
        st = p.stat()
        mtime_iso = datetime.datetime.fromtimestamp(
            st.st_mtime, datetime.timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ")
        size = st.st_size
    except Exception:
        mtime_iso = None
        size = None
    return {
        "path": str(p),
        "sha256": file_sha256(p),
        "mtime_iso": mtime_iso,
        "size_bytes": size,
    }


def env_delta(
    env: Mapping[str, str] | None = None,
    keys: Iterable[str] | None = None,
    prefix: str = _DEFAULT_ENV_PREFIX,
) -> dict[str, str]:
    """MC2_* (or allow-listed) env vars set in `env` (default os.environ).

    If `keys` is given, only those keys are recorded (value or "<unset>").
    Otherwise every var whose name starts with `prefix` is recorded.
    """
    src = dict(os.environ) if env is None else dict(env)
    if keys is not None:
        return {k: src.get(k, "<unset>") for k in keys}
    return {k: v for k, v in sorted(src.items()) if k.startswith(prefix)}


# --------------------------------------------------------------------------- #
# Builders
# --------------------------------------------------------------------------- #

def identity_block(
    *,
    generator: str,
    exe_path: str | os.PathLike | None = None,
    repo_root: str | os.PathLike | None = None,
    deploy_target: str | os.PathLike | None = None,
    env: Mapping[str, str] | None = None,
    env_keys: Iterable[str] | None = None,
    zip_set: list[str] | None = None,
    extra: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the canonical identity block embedded by every species.

    generator:     short tool tag, e.g. "run_smoke", "mc2mod", "trackg_cook",
                   "visual_capture", "release_zip_install".
    exe_path:      mc2.exe / editor exe whose identity gates the artifact (or None).
    repo_root:     worktree root for git metadata (or None to omit git).
    deploy_target: the deploy dir the artifact pertains to (or None).
    env / env_keys: environment to snapshot MC2_* deltas from (default os.environ;
                   env_keys restricts to an explicit allow-list).
    zip_set:       fst/pak/archive set names (release/package species) or None.
    extra:         species-specific extra fields merged at the top level.
    """
    block: dict[str, Any] = {
        "schema": SCHEMA,
        "generator": generator,
        "generated_utc": _utc_now_iso(),
        "host": platform.node() or None,
        "git": git_block(repo_root),
        "exe": exe_block(exe_path),
        "deploy_target": (str(deploy_target) if deploy_target is not None else None),
        "env_delta": env_delta(env=env, keys=env_keys),
        "zip_set": zip_set,
    }
    if extra:
        block.update(dict(extra))
    return block


def report_summary(
    *,
    verdict: str,
    missions: list[str] | None = None,
    artifact_dir: str | os.PathLike | None = None,
    extra: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the canonical report summary (verdict-bearing species only)."""
    out: dict[str, Any] = {
        "verdict": verdict,
        "missions": missions if missions is not None else [],
        "artifact_dir": (str(artifact_dir) if artifact_dir is not None else None),
    }
    if extra:
        out.update(dict(extra))
    return out


def attach(
    manifest: dict[str, Any],
    identity: dict[str, Any],
    report: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Additively embed `identity` (+ optional `report`) into an existing
    manifest dict WITHOUT removing any pre-existing keys. Returns the same
    dict for chaining. This is the back-compat adoption path: a species keeps
    its legacy fields and gains the unified blocks alongside them.
    """
    manifest["identity"] = identity
    if report is not None:
        manifest["report"] = report
    return manifest


# --------------------------------------------------------------------------- #
# Validation
# --------------------------------------------------------------------------- #

_REQUIRED_IDENTITY_KEYS = (
    "schema", "generator", "generated_utc", "git", "exe",
    "deploy_target", "env_delta", "zip_set",
)
_REQUIRED_GIT_KEYS = ("commit", "commit_short", "branch", "dirty", "describe")


def validate_identity(obj: Any) -> list[str]:
    """Return a list of human-readable problems; empty list == conformant.

    Checks shape, not data presence: a null exe/git/deploy_target is allowed
    (some species legitimately have none) -- only the KEYS must exist and the
    schema tag must match. This is the conformance contract the drift checker
    (scripts/check-manifest-schema.py) enforces.
    """
    errs: list[str] = []
    if not isinstance(obj, Mapping):
        return ["identity is not an object"]
    if obj.get("schema") != SCHEMA:
        errs.append(f"schema != {SCHEMA!r} (got {obj.get('schema')!r})")
    for k in _REQUIRED_IDENTITY_KEYS:
        if k not in obj:
            errs.append(f"missing identity key: {k}")
    if not obj.get("generator"):
        errs.append("generator is empty/missing")
    git = obj.get("git")
    if not isinstance(git, Mapping):
        errs.append("git block is not an object")
    else:
        for k in _REQUIRED_GIT_KEYS:
            if k not in git:
                errs.append(f"missing git key: {k}")
    exe = obj.get("exe")
    if exe is not None:
        if not isinstance(exe, Mapping):
            errs.append("exe block is neither null nor an object")
        else:
            for k in ("path", "sha256", "mtime_iso", "size_bytes"):
                if k not in exe:
                    errs.append(f"missing exe key: {k}")
    if not isinstance(obj.get("env_delta"), Mapping):
        errs.append("env_delta is not an object")
    return errs


def is_conformant(obj: Any) -> bool:
    return not validate_identity(obj)


# Convenience flat accessors (the user's named fields), null-safe.

def get_exe_sha(manifest: Mapping[str, Any]) -> str | None:
    ident = manifest.get("identity") or {}
    exe = ident.get("exe") or {}
    return exe.get("sha256")


def get_git_commit(manifest: Mapping[str, Any]) -> str | None:
    ident = manifest.get("identity") or {}
    return (ident.get("git") or {}).get("commit")


def get_verdict(manifest: Mapping[str, Any]) -> str | None:
    return (manifest.get("report") or {}).get("verdict")


if __name__ == "__main__":
    # Self-demo: print an identity block for this repo + the current exe guess.
    import json
    root = Path(__file__).resolve().parents[1]
    ident = identity_block(
        generator="manifest_schema.selfdemo",
        repo_root=root,
        deploy_target=r"A:/Games/mc2-opengl/mc2-win64-v0.4",
    )
    print(json.dumps({"identity": ident}, indent=2))
    problems = validate_identity(ident)
    print("VALID" if not problems else f"INVALID: {problems}")

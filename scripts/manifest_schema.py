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
    identity.binary_provenance.commit
                              <- THE COMMIT THAT WAS ACTUALLY MEASURED, read out
                                 of the binary's own embedded build fingerprint
    identity.source_worktree_head_at_capture.commit
                              <- the tool's worktree HEAD when the artifact was
                                 written. NOT provenance. Also mirrored at the
                                 legacy key `identity.git` for back-compat.
    identity.deploy_target    <- deploy_target
    identity.env_delta        <- env_delta (MC2_* deltas vs a clean env)
    identity.zip_set          <- zip_set (fst/pak set for release/package species)
    report.verdict            <- verdict
    report.missions           <- mission(s)
    report.artifact_dir       <- artifact_dir

A manifest conforms to "mc2-manifest/1" when it carries an `identity` block
that `validate_identity()` accepts. `report` is optional (only run/release/
visual species have a verdict).

CAPTURE-PROVENANCE-TRUTH-1 (2026-07-30) -- WHAT `git` DOES NOT MEAN
-------------------------------------------------------------------
`identity.git` was, and is, `git rev-parse HEAD` in the worktree THE TOOL RAN
FROM. It says nothing about the binary that produced the artifact. Sitting next
to `"dirty": false` it reads as authoritative provenance, and it is not.

Measured 2026-07-30 across 404 manifests on this machine: of the 31 whose
recorded exe still exists byte-identical (so the truth is checkable at all),
28 name a commit the binary was NOT built from. scratch/mc203reg and
scratch/mc203air together record SEVEN distinct `git.commit` values across 23
arms while every one of those arms ran the SAME binary, built from 79360fdb on
branch claude/spotlight-direction-1 -- a commit and a branch that appear in no
manifest. 23 of the 31 disagree on branch, 11 disagree on dirty.

The fix is additive and lives here so every species inherits it:

  * `identity.exe.embedded_fingerprint` -- the binary's OWN stamp, extracted
    from its bytes by scripts/exe_fingerprint.py. This is the only field in the
    manifest that describes the thing that was measured.
  * `identity.binary_provenance` -- that stamp promoted to the top level with a
    reconciliation verdict against the source HEAD.
  * `identity.source_worktree_head_at_capture` -- the honest name for the old
    `git` block. `identity.git` remains as a byte-identical alias so existing
    readers keep working; both now carry `describes` / `describes_binary` /
    `note` fields stating plainly what they are.

Read the EXE, never the generated header (c5d934d5): a failed compile leaves
generated/build_fingerprint.h freshly stamped and the exe stale, so a guard
that reads the header passes while measuring the wrong binary.
"""
from __future__ import annotations

import datetime
import hashlib
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping

# Sibling module, stdlib-only. Emitters import manifest_schema from all over the
# tree, so make the sibling importable regardless of the caller's sys.path.
_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import exe_fingerprint  # noqa: E402

SCHEMA = "mc2-manifest/1"

#: Verbatim in every manifest, next to the source-HEAD block, so a reader who
#: opens the JSON and nothing else cannot mistake it for provenance.
SOURCE_HEAD_NOTE = (
    "SOURCE WORKTREE HEAD at the moment this artifact was written. It does NOT "
    "describe the binary that was measured -- the tool's worktree and the "
    "worktree that compiled the exe are frequently different lanes. For the "
    "commit that was actually exercised, read identity.binary_provenance."
)
SOURCE_DIRTY_NOTE = (
    "'dirty' here = uncommitted TRACKED modifications in the SOURCE WORKTREE at "
    "capture time. It says nothing about whether the measured binary was built "
    "from a clean tree; that is identity.binary_provenance.dirty."
)
BINARY_PROVENANCE_NOTE = (
    "Read out of the binary's own compiled-in build fingerprint (.rdata string "
    "literals stamped by cmake/GenerateBuildFingerprint.cmake). This is the "
    "only commit in this manifest that describes the code that ran."
)

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
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return None


def git_block(repo_root: str | os.PathLike | None) -> dict[str, Any]:
    """The SOURCE WORKTREE HEAD at capture time. NOT the binary's provenance.

    {commit, commit_short, branch, dirty, describe} -- any field may be null --
    plus the self-describing keys `describes` / `describes_binary` /
    `dirty_describes` / `note` (CAPTURE-PROVENANCE-TRUTH-1). Those four exist so
    that a human or a tool reading the raw JSON, with no access to this
    docstring, cannot read `commit` as "the commit that was measured". It is
    `git rev-parse HEAD` in whatever worktree the emitting tool happened to run
    from, which is routinely a different lane from the one that built the exe.

    Unifies the three prior namings: cockpit `git_head` (short),
    write-deploy-manifest `git_head`(full)+`git_branch`+`git_dirty`.
    """
    labels = {
        "describes": "source_worktree_head_at_capture",
        "describes_binary": False,
        "dirty_describes": SOURCE_DIRTY_NOTE,
        "note": SOURCE_HEAD_NOTE,
    }
    if repo_root is None:
        return {"commit": None, "commit_short": None, "branch": None,
                "dirty": None, "describe": None, **labels}
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
        "worktree": str(repo_root),
        **labels,
    }


def exe_block(exe_path: str | os.PathLike | None) -> dict[str, Any] | None:
    """{path, sha256, mtime_iso, size_bytes, embedded_fingerprint}.

    Full sha256 (NOT a prefix). Unifies cockpit `exe.sha256_prefix` (16ch) and
    deploy `files[].sha256` (full) onto one full-hash block. Returns None if
    exe_path is None.

    `embedded_fingerprint` (CAPTURE-PROVENANCE-TRUTH-1) is the binary's own
    compiled-in build stamp, read from its bytes. When the binary carries no
    stamp the key is still present and carries a `reason` -- an absent
    fingerprint must be visible, not a silent null that reads like "clean".

    The file is read exactly once and both the digest and the fingerprint are
    derived from the same buffer, so the two can never describe different bytes.
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
    try:
        blob = p.read_bytes()
    except Exception as e:  # noqa: BLE001
        return {
            "path": str(p), "sha256": None, "mtime_iso": mtime_iso,
            "size_bytes": size,
            "embedded_fingerprint": {
                "present": False, "reason": "unreadable",
                "detail": f"{type(e).__name__}: {e}"},
        }
    fp = exe_fingerprint.extract_from_bytes(blob)
    digest = hashlib.sha256(blob).hexdigest()
    fp["exe_sha256_at_scan"] = digest
    return {
        "path": str(p),
        "sha256": digest,
        "mtime_iso": mtime_iso,
        "size_bytes": size,
        "embedded_fingerprint": fp,
    }


def binary_provenance(exe_blk: Mapping[str, Any] | None,
                      source_head: Mapping[str, Any] | None) -> dict[str, Any]:
    """Promote the exe's own stamp to a top-level, plainly-worded verdict.

    This is the block a reader should reach for when asking "what code produced
    this artifact". It is derived ONLY from the binary; `source_head` is used
    solely to compute the reconciliation verdict, never as a fallback value --
    falling back would reintroduce exactly the misattribution being fixed.

    verdict:
      reconciled            binary commit and source HEAD agree
      SOURCE-HEAD-MISMATCH  they disagree; the source HEAD is not what ran
      unknown               the binary carries no usable stamp, or the source
                            HEAD is unavailable, so nothing can be concluded
    """
    fp = (exe_blk or {}).get("embedded_fingerprint") or {}
    head_sha = (source_head or {}).get("commit")
    if not fp.get("present"):
        return {
            "commit": None,
            "commit_source": "exe-embedded-build-fingerprint",
            "available": False,
            "reason": fp.get("reason", "no-exe-recorded"),
            "detail": fp.get("detail", ""),
            "verdict": "unknown",
            "source_head_matches_binary": None,
            "note": ("This artifact's binary carries no readable build "
                     "fingerprint, so the commit it was built from is NOT "
                     "recorded anywhere and cannot be recovered later. Treat "
                     "any commit attribution for this artifact as unproven."),
        }
    agrees = exe_fingerprint.sha_agrees(fp.get("sha"), head_sha)
    return {
        "commit": fp.get("sha"),
        "commit_source": "exe-embedded-build-fingerprint",
        "available": True,
        "dirty": fp.get("dirty"),
        "dirty_describes": ("uncommitted TRACKED modifications in the worktree "
                            "the BINARY was compiled from, at compile time"),
        "branch": fp.get("branch"),
        "built_utc": fp.get("built"),
        "built_from_worktree": fp.get("src"),
        "method": fp.get("method"),
        "verdict": ("reconciled" if agrees is True
                    else "SOURCE-HEAD-MISMATCH" if agrees is False
                    else "unknown"),
        "source_head_matches_binary": agrees,
        "source_head_commit": head_sha,
        "note": BINARY_PROVENANCE_NOTE,
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
    head = git_block(repo_root)
    exe = exe_block(exe_path)
    block: dict[str, Any] = {
        "schema": SCHEMA,
        "generator": generator,
        "generated_utc": _utc_now_iso(),
        "host": platform.node() or None,
        # WHAT WAS MEASURED. Listed first because it is the field that answers
        # the question every reader of a capture manifest is actually asking.
        "binary_provenance": binary_provenance(exe, head),
        # WHAT THE TOOL'S WORKTREE HAPPENED TO BE ON. Not provenance.
        "source_worktree_head_at_capture": head,
        # Legacy alias, byte-identical to the block above. Kept so existing
        # readers of `identity.git` keep working; deprecated for new code.
        "git": head,
        "exe": exe,
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


# --------------------------------------------------------------------------- #
# Provenance validation (CAPTURE-PROVENANCE-TRUTH-1)
# --------------------------------------------------------------------------- #

#: Species that measure a binary and therefore MUST record which binary.
#: A manifest of one of these kinds without an exe sha256 is not a weak
#: manifest, it is an unattributable measurement.
BINARY_BEARING_KINDS = ("visual-capture", "visual-verify-capture",
                        "visual-compare", "smoke-run", "golden-set")


def validate_provenance(manifest: Mapping[str, Any]) -> list[str]:
    """Problems that make a manifest's provenance untrustworthy. [] == fine.

    Deliberately does NOT check the source HEAD against anything: a manifest
    whose source HEAD differs from the binary's commit is normal and honest so
    long as it SAYS so. What is checked is that the manifest states what it can
    actually know:

      * an exe was recorded at all, with a full sha256;
      * the binary's own fingerprint was recorded, or its absence has a reason;
      * `binary_provenance` exists and does not contradict the exe block.
    """
    errs: list[str] = []
    if not isinstance(manifest, Mapping):
        return ["manifest is not an object"]
    ident = manifest.get("identity")
    if not isinstance(ident, Mapping):
        return ["no identity block -- provenance is unrecorded"]

    exe = ident.get("exe")
    if not isinstance(exe, Mapping):
        return ["identity.exe is absent -- the manifest does not say which "
                "binary it measured"]
    sha = exe.get("sha256")
    if not sha:
        errs.append("identity.exe.sha256 is missing -- the binary is not "
                    "identified, so nothing can be attributed to it later "
                    "(an mtime is not an identity)")
    elif not (isinstance(sha, str) and len(sha) == 64
              and all(c in "0123456789abcdef" for c in sha.lower())):
        errs.append(f"identity.exe.sha256 is not a full sha256: {sha!r}")

    fp = exe.get("embedded_fingerprint")
    if not isinstance(fp, Mapping):
        errs.append("identity.exe.embedded_fingerprint is absent -- the "
                    "binary's own build stamp was never read, so the commit "
                    "this artifact exercised is unrecorded")
    elif not fp.get("present") and not fp.get("reason"):
        errs.append("identity.exe.embedded_fingerprint says neither present "
                    "nor why not -- a silent null reads as 'fine'")
    elif fp.get("present"):
        scanned = fp.get("exe_sha256_at_scan")
        if sha and scanned and scanned != sha:
            errs.append(
                f"identity.exe.embedded_fingerprint was read from different "
                f"bytes than identity.exe.sha256 names "
                f"(scan={scanned[:12]} recorded={sha[:12]})")

    bp = ident.get("binary_provenance")
    if not isinstance(bp, Mapping):
        errs.append("identity.binary_provenance is absent -- the manifest "
                    "never states which commit was measured")
    else:
        if isinstance(fp, Mapping) and bool(bp.get("available")) != bool(fp.get("present")):
            errs.append("identity.binary_provenance.available contradicts "
                        "identity.exe.embedded_fingerprint.present")
        if bp.get("available") and not bp.get("commit"):
            errs.append("identity.binary_provenance claims available but "
                        "records no commit")

    src = ident.get("source_worktree_head_at_capture")
    if isinstance(src, Mapping) and src.get("describes_binary") is not False:
        errs.append("identity.source_worktree_head_at_capture must carry "
                    "describes_binary=false so it cannot be misread as "
                    "provenance")
    legacy = ident.get("git")
    if isinstance(legacy, Mapping) and legacy.get("describes_binary") is not False:
        errs.append("identity.git must carry describes_binary=false -- it is "
                    "the source worktree HEAD, not the binary's commit")
    return errs


# Convenience flat accessors (the user's named fields), null-safe.

def get_exe_sha(manifest: Mapping[str, Any]) -> str | None:
    ident = manifest.get("identity") or {}
    exe = ident.get("exe") or {}
    return exe.get("sha256")


def get_git_commit(manifest: Mapping[str, Any]) -> str | None:
    """DEPRECATED -- returns the SOURCE WORKTREE HEAD, not what was measured.

    Kept because existing callers depend on it. New code that wants "which
    commit produced this artifact" must call get_binary_commit(); this function
    answers a different question and answering it is almost never what a reader
    of a capture manifest intends.
    """
    ident = manifest.get("identity") or {}
    src = ident.get("source_worktree_head_at_capture") or ident.get("git") or {}
    return src.get("commit")


def get_binary_commit(manifest: Mapping[str, Any]) -> str | None:
    """The commit compiled into the measured binary, or None if unknowable.

    None means "this artifact does not record what produced it" -- it never
    means "same as the source HEAD". Callers must not substitute one for the
    other.
    """
    ident = manifest.get("identity") or {}
    bp = ident.get("binary_provenance") or {}
    if bp.get("available"):
        return bp.get("commit")
    fp = ((ident.get("exe") or {}).get("embedded_fingerprint") or {})
    return fp.get("sha") if fp.get("present") else None


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

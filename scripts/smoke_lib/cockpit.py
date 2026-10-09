"""
smoke_lib/cockpit.py -- Post-verdict cockpit artifact writer for S2.

Writes three files into the run's artifact dir after the smoke verdict is
finalized:

    manifest.json       -- run identity (exe, git, env-gates, missions, result)
    oracle_summary.json -- per-oracle tag counts/status lifted from logs
    telemetry.ndjson    -- full NDJSON lifter output for the run

CONTRACT: This module MUST NOT raise any exception that propagates to the
caller.  Every public entry point is wrapped in a top-level try/except that
swallows failures and writes a .cockpit-error.txt sidecar.  The smoke exit
code is computed BEFORE this module is ever called and is never touched here.

Do NOT import from gates.py or baselines.py.  Do NOT modify run_smoke verdict
state.  Python 3 stdlib only.
"""
from __future__ import annotations

import datetime
import hashlib
import json
import os
import subprocess
import sys
import traceback
from pathlib import Path
from typing import Any
from . import nofocus  # noqa: E402  (OWNER RULE: no window for any child process)

# ---------------------------------------------------------------------------
# Internal constants
# ---------------------------------------------------------------------------
_SCHEMA_V = 1
_ERROR_FNAME = ".cockpit-error.txt"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _exe_meta(exe_path: str) -> dict[str, Any]:
    """Return mtime_iso, size_bytes, sha256_hex (first 16 chars) for exe."""
    p = Path(exe_path)
    try:
        st = p.stat()
        mtime_iso = datetime.datetime.utcfromtimestamp(st.st_mtime).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )
        size = st.st_size
    except OSError:
        mtime_iso = None
        size = None

    sha256 = None
    try:
        h = hashlib.sha256()
        with open(p, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 16), b""):
                h.update(chunk)
        sha256 = h.hexdigest()[:16]
    except OSError:
        pass

    return {"path": str(p), "mtime_iso": mtime_iso, "size_bytes": size, "sha256_prefix": sha256}


def _git_head(repo_root: str) -> str | None:
    """Return the short git HEAD sha for the scripts repo."""
    try:
        result = subprocess.run(
            ["git", "-C", repo_root, "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=5,
            **nofocus.hidden_kwargs()
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return None


def _env_gate_snapshot() -> dict[str, str]:
    """Return all MC2_* env vars that are currently set."""
    return {k: v for k, v in os.environ.items() if k.startswith("MC2_")}


# ---------------------------------------------------------------------------
# oracle_summary builder
# ---------------------------------------------------------------------------

def _build_oracle_summary(records: list[dict], registry: dict) -> list[dict]:
    """
    Produce per-oracle tag summary from lifted records + registry.

    For each tag in registry with kind=="oracle":
      - Collect all matching records
      - Identify fields_must_be_zero (from registry)
      - Determine status: "pass" (all zero), "fail" (any nonzero), "vacuous" (no records)

    Returns list of dicts ordered by tag name.
    """
    # Group records by tag
    by_tag: dict[str, list[dict]] = {}
    for rec in records:
        tag = rec.get("tag", "")
        by_tag.setdefault(tag, []).append(rec)

    summaries = []
    for tag, reg_entry in sorted(registry.items()):
        if reg_entry.get("kind") != "oracle":
            continue

        tag_records = by_tag.get(tag, [])
        fields_must_zero = reg_entry.get("fields_must_be_zero", [])
        # Collect per-mission breakdown
        missions_seen: dict[str, int] = {}
        max_violations: dict[str, int] = {}
        total_records = len(tag_records)
        violation_count = 0

        for rec in tag_records:
            mission = rec.get("fields", {}).get("_mission", "unknown")
            missions_seen[mission] = missions_seen.get(mission, 0) + 1
            for field in fields_must_zero:
                val = rec.get("fields", {}).get(field)
                if val is None:
                    continue
                try:
                    ival = int(val)
                except (TypeError, ValueError):
                    try:
                        ival = int(float(val))
                    except (TypeError, ValueError):
                        continue
                if ival != 0:
                    violation_count += 1
                    max_violations[field] = max(max_violations.get(field, 0), ival)

        if total_records == 0:
            status = "vacuous"
        elif violation_count > 0:
            status = "fail"
        else:
            status = "pass"

        summaries.append({
            "tag": tag,
            "kind": "oracle",
            "total_records": total_records,
            "missions": missions_seen,
            "fields_must_be_zero": fields_must_zero,
            "max_violations": max_violations,
            "violation_count": violation_count,
            "status": status,
            "doc": reg_entry.get("doc"),
        })

    return summaries


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------

def write_cockpit_artifacts(
    artifact_dir: Path | str,
    *,
    exe_path: str,
    tier: str,
    profile: str,
    missions: list[str],
    durations: dict[str, int],
    result: str,
    repo_root: Path | str | None = None,
    registry_path: Path | str | None = None,
    source: str = "smoke",
) -> None:
    """
    Write manifest.json, oracle_summary.json, and telemetry.ndjson into
    artifact_dir.  Any exception is caught internally and written to
    .cockpit-error.txt; this function never raises.

    Args:
        artifact_dir: the run's artifact directory (already exists).
        exe_path:     path to mc2.exe that was used.
        tier:         smoke tier string (e.g. "tier1").
        profile:      profile string (e.g. "stock").
        missions:     list of mission stems that were run.
        durations:    dict stem -> duration_s.
        result:       overall result string ("PASS" or "FAIL").
        repo_root:    repo root for git HEAD lookup (default: derived from __file__).
        registry_path: path to tag-registry.json (default: auto-located).
        source:       telemetry source label.
    """
    artifact_dir = Path(artifact_dir)
    error_path = artifact_dir / _ERROR_FNAME

    try:
        _write_cockpit_artifacts_inner(
            artifact_dir=artifact_dir,
            exe_path=exe_path,
            tier=tier,
            profile=profile,
            missions=missions,
            durations=durations,
            result=result,
            repo_root=repo_root,
            registry_path=registry_path,
            source=source,
        )
    except Exception:
        tb = traceback.format_exc()
        try:
            error_path.write_text(
                f"[cockpit] artifact write failed at "
                f"{datetime.datetime.utcnow().isoformat()}Z\n\n{tb}",
                encoding="utf-8",
            )
        except Exception:
            pass  # truly swallowed; never propagate


def _write_cockpit_artifacts_inner(
    artifact_dir: Path,
    exe_path: str,
    tier: str,
    profile: str,
    missions: list[str],
    durations: dict[str, int],
    result: str,
    repo_root: Any,
    registry_path: Any,
    source: str,
) -> None:
    """Inner implementation -- may raise; caller wraps in try/except."""
    # Resolve repo root
    if repo_root is None:
        # scripts/smoke_lib/cockpit.py -> scripts/smoke_lib -> scripts -> repo_root
        repo_root = Path(__file__).resolve().parents[2]
    else:
        repo_root = Path(repo_root)

    # Resolve registry
    if registry_path is None:
        registry_path = repo_root / "tests" / "telemetry" / "tag-registry.json"
    else:
        registry_path = Path(registry_path)

    # ------------------------------------------------------------------
    # 1. manifest.json
    # ------------------------------------------------------------------
    manifest = {
        "schema_v": _SCHEMA_V,
        "timestamp_iso": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
        "tier": tier,
        "profile": profile,
        "missions": missions,
        "durations_s": durations,
        "exe": _exe_meta(exe_path),
        "git_head": _git_head(str(repo_root)),
        "env_gates": _env_gate_snapshot(),
        "result": result,
        "source": source,
    }
    # S12: additively embed the unified identity + report blocks (mc2-manifest/1)
    # alongside the legacy fields. Consumers that read schema_v/exe/git_head keep
    # working; new consumers join on identity.*/report.*. Best-effort -- a
    # missing manifest_schema module must not break the (sacred) cockpit writer.
    try:
        scripts_dir = str(Path(repo_root) / "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        import manifest_schema  # noqa: E402
        ident = manifest_schema.identity_block(
            generator="run_smoke",
            exe_path=exe_path,
            repo_root=str(repo_root),
            deploy_target=str(Path(exe_path).parent),
        )
        rep = manifest_schema.report_summary(
            verdict=result, missions=missions, artifact_dir=str(artifact_dir),
        )
        manifest_schema.attach(manifest, ident, rep)
    except Exception:
        pass  # legacy manifest still written; identity enrichment is optional
    (artifact_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )

    # ------------------------------------------------------------------
    # 2. telemetry.ndjson -- invoke the lifter
    # ------------------------------------------------------------------
    # Import telemetry_lift from the scripts package.  It lives at
    # scripts/telemetry_lift.py; sys.path already includes repo_root
    # (run_smoke.py inserts it), but guard with an explicit insert.
    scripts_dir = str(repo_root / "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)

    # Import at call time so the module is not required at import.
    try:
        import importlib
        telemetry_lift = importlib.import_module("telemetry_lift")
    except ImportError:
        # Try the package-relative path.
        import importlib.util
        lift_path = repo_root / "scripts" / "telemetry_lift.py"
        spec = importlib.util.spec_from_file_location("telemetry_lift", lift_path)
        telemetry_lift = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(telemetry_lift)

    registry = telemetry_lift.load_registry(str(registry_path))
    session = artifact_dir.name  # e.g. "2026-06-11T14-22-00"
    records = telemetry_lift.lift_artifact_dir(
        str(artifact_dir), registry, session=session, source=source
    )

    ndjson_path = artifact_dir / "telemetry.ndjson"
    with open(ndjson_path, "w", encoding="utf-8") as fh:
        for rec in records:
            fh.write(json.dumps(rec, separators=(",", ":")) + "\n")

    # ------------------------------------------------------------------
    # 3. oracle_summary.json
    # ------------------------------------------------------------------
    oracle_summaries = _build_oracle_summary(records, registry)
    oracle_summary = {
        "schema_v": _SCHEMA_V,
        "session": session,
        "tier": tier,
        "profile": profile,
        "result": result,
        "oracle_count": len(oracle_summaries),
        "pass_count": sum(1 for s in oracle_summaries if s["status"] == "pass"),
        "fail_count": sum(1 for s in oracle_summaries if s["status"] == "fail"),
        "vacuous_count": sum(1 for s in oracle_summaries if s["status"] == "vacuous"),
        "oracles": oracle_summaries,
    }
    (artifact_dir / "oracle_summary.json").write_text(
        json.dumps(oracle_summary, indent=2), encoding="utf-8"
    )

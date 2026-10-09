"""SMOKE-CRASH-SILENT-EVIDENCE-1 — runner-only evidence capture.

When a mission ends in a `crash_silent` / `heartbeat_freeze_*` / `crash_no_summary`
verdict, gather cheap environment evidence so the (currently INCONCLUSIVE /
ENVIRONMENT-SUSPECTED) flake can be CLASSIFIED instead of guessed — see
docs/testing/smoke-first-launch-crash-silent-recon-1.md.

This module makes NO engine change and NO smoke-timing change. It only reads the
already-collected RunResult plus a few best-effort OS probes, and writes
`<stem>.crash_evidence.json` into the artifact dir. EVERY probe is wrapped so a
failing probe (no rights, slow WMI, missing tool) degrades to a recorded error
and NEVER affects the smoke verdict. The caller invokes capture() only after the
verdict is already decided.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional
from . import nofocus  # noqa: E402  (OWNER RULE: no window for any child process)

# Verdict buckets that warrant evidence capture (external/non-app terminations
# and freezes — the ones this recon is about).
EVIDENCE_BUCKETS = {
    "crash_silent",
    "crash_no_summary",
    "heartbeat_freeze_play",
    "heartbeat_freeze_load",
}


def _ps(cmd: str, timeout: float) -> Optional[str]:
    """Run a PowerShell one-liner, return stdout or None. Best-effort."""
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
            text=True, stderr=subprocess.DEVNULL, timeout=timeout,
            **nofocus.hidden_kwargs())
        return (out or "").strip()
    except Exception:
        return None


def _tail(text: str, n: int) -> List[str]:
    lines = (text or "").splitlines()
    return lines[-n:] if len(lines) > n else lines


def _heartbeat_tail(text: str, n: int) -> List[str]:
    hb = [ln for ln in (text or "").splitlines() if "[HEARTBEAT]" in ln]
    return hb[-n:] if len(hb) > n else hb


def _find_minidumps(exe_dir: Path) -> List[str]:
    """Recent crash artifacts next to the exe (best-effort, non-recursive)."""
    hits: List[str] = []
    try:
        for p in exe_dir.iterdir():
            if not p.is_file():
                continue
            nm = p.name.lower()
            if nm.endswith((".dmp", ".mdmp")) or "crash" in nm or "minidump" in nm:
                hits.append(p.name)
    except Exception:
        pass
    return hits


def _event_log_hints(timeout: float) -> Any:
    """Windows event-log hints in the last ~5 min: display-driver TDR / reset
    (System log, e.g. event 4101 'Display driver ... stopped responding'),
    Application Error (1000), and Windows Error Reporting (1001)."""
    # One compact query; JSON so we can parse. Keep the window small + capped.
    cmd = (
        "$since=(Get-Date).AddMinutes(-5);"
        "$ids=@(1000,1001,4101);"
        "try { Get-WinEvent -FilterHashtable @{LogName=@('System','Application');"
        "StartTime=$since} -ErrorAction SilentlyContinue |"
        " Where-Object { $ids -contains $_.Id -or $_.ProviderName -match"
        " 'nvlddmkm|amdkmdag|igfx|Display|Application Error|Windows Error Reporting' } |"
        " Select-Object -First 15 TimeCreated,Id,ProviderName,"
        "@{n='Message';e={ ($_.Message -split \"`n\")[0] }} |"
        " ConvertTo-Json -Compress } catch { '[]' }"
    )
    out = _ps(cmd, timeout)
    if not out:
        return {"queried": True, "events": [], "note": "no matching events / query failed"}
    try:
        data = json.loads(out)
        if isinstance(data, dict):
            data = [data]
        return {"queried": True, "events": data}
    except Exception:
        return {"queried": True, "events": [], "raw": out[:2000]}


def _gpu_info(timeout: float) -> Any:
    cmd = ("Get-CimInstance Win32_VideoController |"
           " Select-Object Name,DriverVersion,DriverDate | ConvertTo-Json -Compress")
    out = _ps(cmd, timeout)
    if not out:
        return None
    try:
        return json.loads(out)
    except Exception:
        return {"raw": out[:1000]}


def capture(stem: str,
            result: Any,
            exe_path: Path,
            artifact_dir: Path,
            enum_procs_fn: Optional[Callable[[], list]] = None,
            event_log_timeout: float = 8.0) -> Dict[str, Any]:
    """Gather evidence for a flaky/failed mission and write
    <stem>.crash_evidence.json. Returns the dict. Best-effort; never raises for a
    probe failure (only a genuinely broken caller arg would surface)."""
    summary = getattr(result, "summary", None)
    mission_ready_ms = getattr(summary, "mission_ready_ms", None)
    phase = "play" if mission_ready_ms is not None else "load_or_pre"

    exe_dir = Path(exe_path).resolve().parent

    # Concurrent mc2.exe — the contention hypothesis. Reuse the runner's enum.
    concurrent: Any
    try:
        concurrent = enum_procs_fn() if enum_procs_fn else "enum_fn_not_provided"
        if isinstance(concurrent, list):
            concurrent = [{"pid": p, "path": pth} for (p, pth, *_rest) in concurrent]
    except Exception as exc:  # noqa: BLE001
        concurrent = {"error": str(exc)}

    evidence: Dict[str, Any] = {
        "schema": "crash_evidence/1",
        "mission": stem,
        "buckets": list(getattr(result, "verdict").buckets),
        "exit_code": getattr(result, "exit_code", None),
        "walltime_s": round(getattr(result, "walltime_s", 0.0), 2),
        "killed_by_timeout": getattr(result, "killed_by_timeout", None),
        # phase / timing
        "mission_phase": phase,
        "mission_ready_ms": mission_ready_ms,
        "last_heartbeat_wall_s_play": getattr(summary, "last_heartbeat_wall_s_play", None),
        "last_heartbeat_wall_s_load": getattr(summary, "last_heartbeat_wall_s_load", None),
        # app-crash signal (False is the crash_silent signature)
        "crash_handler_hit": getattr(summary, "crash_handler_hit", None),
        "minidumps_near_exe": _find_minidumps(exe_dir),
        # environment probes (best-effort)
        "concurrent_mc2": concurrent,
        "windows_event_log": _event_log_hints(event_log_timeout),
        "gpu": _gpu_info(5.0),
        # raw tails for human inspection
        "stdout_tail": _tail(getattr(result, "stdout_text", ""), 40),
        "heartbeat_tail": _heartbeat_tail(getattr(result, "stdout_text", ""), 6),
    }

    try:
        (Path(artifact_dir) / f"{stem}.crash_evidence.json").write_text(
            json.dumps(evidence, indent=2), encoding="utf-8")
    except Exception as exc:  # noqa: BLE001
        evidence["_write_error"] = str(exc)
    return evidence

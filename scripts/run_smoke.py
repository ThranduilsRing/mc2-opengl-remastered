#!/usr/bin/env python3
# scripts/run_smoke.py
"""MC2 smoke matrix runner.

Examples:
  python scripts/run_smoke.py --tier tier1 --fail-fast
  python scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
  python scripts/run_smoke.py --tier tier2
  python scripts/run_smoke.py --mission mc2_01 --mission mc2_24
  python scripts/run_smoke.py --menu-canary

WARNING: --with-menu-canary and --kill-existing are both DANGEROUS in normal
use (kill-existing can kill a concurrent mc2.exe and produce a false
crash_silent; with-menu-canary/long durations are not part of the standard
smoke gate) -- do not add them to routine invocations.
"""
from __future__ import annotations

import argparse
import atexit
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib import baselines, manifest, nofocus, report
from scripts.smoke_lib.gates import Verdict as _Verdict
from scripts.smoke_lib.logparse import LogSummary as _LogSummary
from scripts.smoke_lib.runner import RunConfig, run_one
from scripts.smoke_lib import runner as _smoke_runner

# HARNESS-HYGIENE-3: what the run was ACTUALLY configured with. Ambient MC2_*
# vars reach the engine (see runner.py's ENV COMPOSITION CONTRACT), so the run
# is only self-describing if we write them down. Emitted into report.json.
_env_manifest: dict = {}
# Cockpit is strictly post-verdict and must never affect the smoke verdict,
# including via an import-time failure (e.g. SyntaxError in cockpit.py).
try:
    from scripts.smoke_lib import cockpit as _cockpit
except Exception:
    _cockpit = None
# Deploy-fingerprint check. DEFAULT = HARD FAIL on mismatch/absence
# (DEPLOY-FINGERPRINT-DEFAULT-ON-1, e01e8ac1); opt OUT with
# MC2_SMOKE_REQUIRE_FINGERPRINT=0. See the _fp_require site in main().
#
# SMOKE-DEPLOY-TRAP-1: the import stays exception-safe so this module can be
# imported by contract checkers on a machine with a broken smoke_lib, but a
# failed import is NOT silently tolerated at run time. If the module is
# unavailable the fingerprint check cannot run at all, and a run that cannot
# verify which exe it smoked must not be able to print PASS -- that is the
# whole defect this guard exists to close, reachable by a 3-line import break.
# main() promotes the import failure to a hard abort (exit 2) unless the
# operator has explicitly opted out.
try:
    from scripts.smoke_lib import fingerprint as _fingerprint
    _fingerprint_import_error = None
except Exception as _fp_import_exc:  # pragma: no cover - defensive
    _fingerprint = None
    _fingerprint_import_error = _fp_import_exc
# SMOKE-CRASH-SILENT-EVIDENCE-1: post-verdict environment evidence capture for
# crash_silent / heartbeat_freeze. Import-safe; never affects the verdict.
try:
    from scripts.smoke_lib import crash_evidence as _crash_evidence
except Exception:
    _crash_evidence = None

# No default exe path: callers must either pass --exe explicitly or let the
# deploy-lease system auto-select the least-recently-used deploy folder. This
# avoids drift toward a stale fixed default and keeps load balanced across
# 0.4 / 0.4c / 0.4d-rc1 / 0.5.0 / 0.5-testing.
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"
MANIFEST_PATH = ROOT / "tests" / "smoke" / "smoke_missions.txt"
BASELINE_PATH = ROOT / "tests" / "smoke" / "baselines.json"
DEFAULT_MENU_SCRIPT = ROOT / "tests" / "smoke" / "menu_canary_first_mission.txt"
GAME_AUTO = ROOT / "scripts" / "game_auto.py"


def _parse_veg_summary(stdout_text: str) -> dict | None:
    """Parse VEG_SUMMARY line from captured engine output.

    Returns a dict with keys instance_count (int), draw_calls (int),
    flush_reason (str) if found; None if the line is absent.

    Format emitted by GosVegetation::emitSummary():
      VEG_SUMMARY instance_count=N draw_calls=N flush_reason=<reason>
    """
    import re as _re
    _VEG_RE = _re.compile(
        r"VEG_SUMMARY\s+"
        r"instance_count=(?P<ic>\d+)\s+"
        r"draw_calls=(?P<dc>\d+)\s+"
        r"flush_reason=(?P<fr>\S+)"
    )
    for line in reversed(stdout_text.splitlines()):
        m = _VEG_RE.search(line)
        if m:
            return {
                "instance_count": int(m.group("ic")),
                "draw_calls":     int(m.group("dc")),
                "flush_reason":   m.group("fr"),
            }
    return None


def _check_veg_floor(stem: str, veg: dict | None) -> list[str]:
    """VEG-SMOKE-FLOOR-1: assert veg output contract for a single mission.

    Returns a list of failure messages (empty = pass).
    Only called when MC2_VEGETATION_CARDS=1.

    Floors:
      mc2_01: instance_count > 100000  (dense forest mission)
      others: instance_count > 1000    (drier terrain — "not zero")
    """
    if veg is None:
        return [f"VEG-SMOKE-FLOOR FAIL {stem}: VEG_SUMMARY line absent from log "
                f"(engine may not have called emitSummary, or MC2_LOG not set)"]

    floor = 100000 if stem == "mc2_01" else 1000
    fails = []
    if veg["instance_count"] <= floor:
        fails.append(
            f"VEG-SMOKE-FLOOR FAIL {stem}: "
            f"instance_count={veg['instance_count']} draw_calls={veg['draw_calls']} "
            f"flush_reason={veg['flush_reason']} "
            f"(instance_count must be > {floor})")
    if veg["draw_calls"] <= 0:
        fails.append(
            f"VEG-SMOKE-FLOOR FAIL {stem}: "
            f"instance_count={veg['instance_count']} draw_calls={veg['draw_calls']} "
            f"flush_reason={veg['flush_reason']} "
            f"(draw_calls must be > 0)")
    if veg["flush_reason"] != "submitted":
        fails.append(
            f"VEG-SMOKE-FLOOR FAIL {stem}: "
            f"instance_count={veg['instance_count']} draw_calls={veg['draw_calls']} "
            f"flush_reason={veg['flush_reason']} "
            f"(flush_reason must be 'submitted')")
    return fails


def _parse_order_liveness_summary(stdout_text: str) -> dict | None:
    """Parse the ORDER_LIVENESS_SUMMARY line (META-FIX design F fixture,
    code/mission.cpp under MC2_FIXTURE_ORDER_LIVENESS + MC2_SOAK_AUTOWIN).

    Format (reviewed grammar — contract, do not normalize):
      ORDER_LIVENESS_SUMMARY issued_move=N moved=N refused=N stalled=N verdict=PASS|FAIL

    Returns dict(issued_move, moved, refused, stalled, verdict) or None.
    """
    import re as _re
    _OL_RE = _re.compile(
        r"ORDER_LIVENESS_SUMMARY\s+"
        r"issued_move=(?P<im>\d+)\s+"
        r"moved=(?P<mv>\d+)\s+"
        r"refused=(?P<rf>\d+)\s+"
        r"stalled=(?P<st>\d+)\s+"
        r"verdict=(?P<v>\w+)"
    )
    for line in reversed(stdout_text.splitlines()):
        m = _OL_RE.search(line)
        if m:
            return {
                "issued_move": int(m.group("im")),
                "moved":       int(m.group("mv")),
                "refused":     int(m.group("rf")),
                "stalled":     int(m.group("st")),
                "verdict":     m.group("v"),
            }
    return None


def _check_order_liveness_floor(stem: str, ol: dict | None) -> list[str]:
    """META-FIX design F runner floor (VEG-SMOKE-FLOOR clone): only called
    when MC2_FIXTURE_ORDER_LIVENESS is set in the runner env. Absence of the
    summary IS a failure — it catches the fixture itself dying silently."""
    if ol is None:
        return [f"ORDER-LIVENESS FAIL {stem}: ORDER_LIVENESS_SUMMARY line "
                f"absent from log (fixture never reached its summary phase — "
                f"needs MC2_SOAK_AUTOWIN and a >=10s autowin window)"]
    fails = []
    if ol["verdict"] != "PASS":
        fails.append(
            f"ORDER-LIVENESS FAIL {stem}: verdict={ol['verdict']} "
            f"issued_move={ol['issued_move']} moved={ol['moved']} "
            f"refused={ol['refused']} stalled={ol['stalled']} "
            f"(stalled>0 = an ordered unit neither moved >=25m nor produced "
            f"a classified refusal — the CLASS-F silent dead-end)")
    return fails


def _norm_path(p: str) -> str:
    """Normalize an exe path for comparison: resolve, forward-slash, casefold.
    Windows paths are case-insensitive and may mix slashes; an unresolvable
    string (e.g. empty ExecutablePath) yields ''."""
    if not p:
        return ""
    try:
        return str(Path(p).resolve()).replace("\\", "/").casefold()
    except Exception:
        return p.replace("\\", "/").casefold()


def _enum_mc2_processes() -> list[tuple[int, str, str]]:
    """Return (pid, exe_path, command_line) for every running mc2.exe via
    Win32_Process metadata. ExecutablePath/CommandLine may be empty on PIDs
    we lack rights to query; callers fall back to CommandLine matching.

    This is the impure half (shells PowerShell). The pure path-discrimination
    logic lives in _same_path_mc2 below, which is what the unit test exercises."""
    ps = (
        "Get-CimInstance Win32_Process -Filter \"Name='mc2.exe'\" | "
        "ForEach-Object { "
        "[pscustomobject]@{pid=$_.ProcessId;path=$_.ExecutablePath;cmd=$_.CommandLine} } | "
        "ConvertTo-Json -Compress"
    )
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
            text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    out = (out or "").strip()
    if not out:
        return []
    try:
        data = json.loads(out)
    except Exception:
        return []
    if isinstance(data, dict):
        data = [data]
    procs = []
    for d in data:
        try:
            pid = int(d.get("pid"))
        except Exception:
            continue
        procs.append((pid, d.get("path") or "", d.get("cmd") or ""))
    return procs


# Backwards-compat alias (older callers / tooling may import this name).
_all_mc2_procs = _enum_mc2_processes


def _exe_from_cmdline(cmd: str) -> str:
    """Extract the leading executable path token from a process command line.

    Handles a leading quoted path (`"C:\\dir\\mc2.exe" arg ...`) and an
    unquoted path with no embedded spaces (`C:\\dir\\mc2.exe arg ...`). Returns
    '' when no token can be isolated. This lets the CommandLine fallback in
    _same_path_mc2 do an EXACT exe-path comparison instead of a substring test
    (which made `mc2-win64-v0.4` spuriously match inside `mc2-win64-v0.4c`)."""
    if not cmd:
        return ""
    s = cmd.strip()
    if not s:
        return ""
    if s[0] in ('"', "'"):
        q = s[0]
        end = s.find(q, 1)
        return s[1:end] if end != -1 else ""
    # Unquoted: the exe is the token up to the first whitespace. Our deploy
    # paths contain no spaces; a space-bearing unquoted path is unrecoverable
    # and correctly yields a non-matching token (treated as foreign).
    return s.split(None, 1)[0]


def _same_path_mc2(procs: list[tuple[int, str, str]],
                   target_exe) -> list[tuple[int, str]]:
    """PURE filter: given (pid, exe_path, cmd) tuples, return only those PIDs
    whose exe was launched from OUR target_exe path.

    Normalizes both sides (resolve -> forward-slash -> casefold). For PIDs with
    an empty ExecutablePath (access-denied), reconstructs the exe path from the
    CommandLine (_exe_from_cmdline) and EXACT-matches it against the target exe.
    No substring/dir-prefix test: a `mc2-win64-v0.4` target must NOT block a
    running `mc2-win64-v0.4c` instance — only the SAME deploy exe blocks.
    If a null-path PID yields no usable CommandLine exe token, it is treated as
    FOREIGN (NOT blocked) -- we never block on an unknown process.
    Foreign mc2.exe (a different deploy path) are never returned.

    Takes the process list as an argument so it can be unit-tested with
    synthetic inputs without launching anything."""
    tgt = _norm_path(str(target_exe))
    matched = []
    for pid, path, cmd in procs:
        npath = _norm_path(path)
        if npath:
            if npath == tgt:
                matched.append((pid, path))
            continue
        # ExecutablePath empty -> reconstruct the exe path from the CommandLine
        # and EXACT-match it. No substring/dir-prefix test, so a v0.4 target
        # never blocks a running v0.4c instance. Only the same deploy exe blocks.
        ncmd_exe = _norm_path(_exe_from_cmdline(cmd))
        if ncmd_exe and ncmd_exe == tgt:
            matched.append((pid, cmd))
        # else: different deploy path / no usable hint -> FOREIGN (not blocked).
    return matched


def _running_mc2(target_exe: Path) -> list[tuple[int, str]]:
    """PIDs of mc2.exe instances launched from OUR target exe path.
    Thin wrapper = filter(enum()). Path-discrimination lives in _same_path_mc2."""
    return _same_path_mc2(_enum_mc2_processes(), target_exe)


def _lane_lock_token(lease_folder: str | None, exe_path: str,
                     short_name_fn) -> str:
    """PURE: derive the filesystem-safe lane token used to name the per-lane
    smoke.<token>.lock file (SMOKE-LATENCY-WINS-1 win #2).

    lease_folder: the folder path we hold a lease on, or None (no lease --
      --no-lease or lease system unavailable).
    exe_path: args.exe (used as the fallback lane identity when there's no
      lease, so two --no-lease invocations pointed at the SAME exe still
      serialize against each other).
    short_name_fn: deploy_lease._short_name (injected so this can be unit
      tested without importing the real deploy_lease module's DEPLOY_FOLDERS
      global state).

    Two invocations resolving to the SAME lane token must serialize (same
    lock file); two resolving to DIFFERENT tokens must run concurrently
    (different lock files). Sanitizes to [A-Za-z0-9_-] so short names
    containing '.' (e.g. "0.4") or an arbitrary directory name can't break
    the lock filename.
    """
    if lease_folder is not None:
        lane_key = short_name_fn(lease_folder)
    else:
        lane_key = Path(exe_path).resolve().parent.name or "default"
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in lane_key)


def _taskkill_mc2(pids: list[int]):
    """Kill ONLY the given PIDs (never /IM image-name kill, which would also
    nuke foreign mc2.exe from a different deploy path)."""
    for pid in pids:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)


def _run_menu_canary(exe: Path, script_path: Path, artifact_dir: Path,
                     keep_logs: bool, settle_s: int) -> int:
    env = os.environ.copy()
    for var in ["MC2_SMOKE_MODE", "MC2_HEARTBEAT", "MC2_SMOKE_SEED"]:
        env.pop(var, None)
    env["MC2_MENU_CANARY_SKIP_INTRO"] = "1"
    # The MC2_SMOKE_MODE pop above is deliberate (the canary must exercise the
    # real menu path), but it also drops the engine-side cursor/activation
    # suppression that smoke mode implies. nofocus.apply_env re-adds
    # MC2_NO_CURSOR_GRAB=1, which is what actually stops SDL from activating
    # its window; the STARTUPINFO only covers the early-boot/console window.
    exe_dir = str(exe.resolve().parent)
    proc = nofocus.spawn(
        [str(exe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=exe_dir,
        env=env,
    )

    start_wall = time.monotonic()
    time.sleep(1.0)
    exited_before_replay = proc.poll() is not None
    auto = subprocess.run(
        [sys.executable, str(GAME_AUTO), "script", str(script_path)],
        text=True,
        capture_output=True,
        cwd=str(ROOT),
    )
    replay_elapsed_s = time.monotonic() - start_wall
    time.sleep(max(0, settle_s))

    game_alive = proc.poll() is None
    if game_alive:
        try:
            proc.wait(timeout=max(1, settle_s))
        except subprocess.TimeoutExpired:
            proc.kill()
    try:
        stdout, _ = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate(timeout=2)
    exit_code = proc.returncode
    lowered = (stdout or "").lower()
    clean_exit = "[exit] gos_terminateapplication called" in lowered
    crashy = any(token in lowered for token in [
        "unhandled exception",
        "fatal error",
        "access violation",
        "stack overflow",
        "abort",
        "assert",
    ])
    early_exit = exit_code == 0 and replay_elapsed_s > 0 and not game_alive and exited_before_replay
    passed = (
        auto.returncode == 0
        and clean_exit
        and not crashy
        and exit_code == 0
        and not early_exit
    )

    md = [
        "# Menu Canary",
        "",
        f"- script: `{script_path.name}`",
        f"- replay_exit: `{auto.returncode}`",
        f"- replay_elapsed_s: `{replay_elapsed_s:.2f}`",
        f"- exited_before_replay: `{str(exited_before_replay).lower()}`",
        f"- game_alive_after_replay: `{str(game_alive).lower()}`",
        f"- game_exit_code: `{exit_code}`",
        f"- clean_exit_marker: `{str(clean_exit).lower()}`",
        f"- crash_signature: `{str(crashy).lower()}`",
        f"- early_exit: `{str(early_exit).lower()}`",
        f"- result: `{'PASS' if passed else 'FAIL'}`",
    ]
    report_text = "\n".join(md) + "\n"
    (artifact_dir / "menu_canary_report.md").write_text(report_text, encoding="utf-8")
    if keep_logs or not passed:
        (artifact_dir / "menu_canary_game.log").write_text(stdout or "", encoding="utf-8", errors="replace")
        (artifact_dir / "menu_canary_replay.log").write_text(
            (auto.stdout or "") + ("\n" if auto.stdout and auto.stderr else "") + (auto.stderr or ""),
            encoding="utf-8",
            errors="replace",
        )
    sys.stdout.buffer.write(report_text.encode("utf-8"))
    sys.stdout.buffer.flush()
    return 0 if passed else 1


def _modmission_env_guard(e, deploy_dir: Path, caller_env: dict) -> tuple[bool, dict, str]:
    """SMOKE-MODMISSION-ENV-GUARDS-1: pre-launch guard for mod missions.

    Returns (ok, env_overrides, error_msg).
      ok=True  -> safe to launch; env_overrides must be merged into env_extra.
      ok=False -> do NOT launch; error_msg is the single-line error to print.

    Guards applied (only when e.mod is non-empty):
      G1 AUTO-SET: Set MC2_ACTIVE_MOD/MC2_MOD_DEPS from manifest.  If caller
         already set them and they DISAGREE, prefer manifest and log a warning.
      G2 VALIDATE: mods/<mod>/data/missions/<stem>.fit must exist in deploy dir.
         Also checks each dep dir exists; warns if absent.
      G3 STALE-CACHE: Deletes mods/<mod>/.modindex-cache (and each dep's) in
         deploy dir before launch to prevent the copied-deploy path staleness class.
    """
    if not e.mod:
        return True, {}, ""

    env_overrides: dict = {}
    # G1: auto-set MC2_ACTIVE_MOD
    caller_mod = caller_env.get("MC2_ACTIVE_MOD", "")
    if caller_mod and caller_mod != e.mod:
        print(f"[runner] [ENV-GUARD] WARNING: caller MC2_ACTIVE_MOD={caller_mod!r} "
              f"disagrees with manifest mod={e.mod!r} for mission {e.stem!r} — "
              f"using manifest value", file=sys.stderr)
    env_overrides["MC2_ACTIVE_MOD"] = e.mod

    # G1: auto-set MC2_MOD_DEPS
    deps_list = [d.strip() for d in e.deps.split(",") if d.strip()] if e.deps else []
    if deps_list:
        caller_deps = caller_env.get("MC2_MOD_DEPS", "")
        manifest_deps = ",".join(deps_list)
        if caller_deps and caller_deps != manifest_deps:
            print(f"[runner] [ENV-GUARD] WARNING: caller MC2_MOD_DEPS={caller_deps!r} "
                  f"disagrees with manifest deps={manifest_deps!r} for mission "
                  f"{e.stem!r} — using manifest value", file=sys.stderr)
        env_overrides["MC2_MOD_DEPS"] = manifest_deps

    # G2: validate mod dir + .fit file exist in deploy
    mod_dir = deploy_dir / "mods" / e.mod
    if not mod_dir.is_dir():
        msg = (f"[runner] [ENV-GUARD] mod dir not found: {mod_dir} for mod={e.mod!r} "
               f"(deploy={deploy_dir}) — check deploy/mod, NOT a code crash")
        return False, {}, msg

    fit_path = mod_dir / "data" / "missions" / f"{e.stem}.fit"
    if not fit_path.is_file():
        msg = (f"[runner] [ENV-GUARD] mission '{e.stem}' not found at {fit_path} "
               f"for mod={e.mod!r} (deploy={deploy_dir}) — check deploy/mod, NOT a code crash")
        return False, {}, msg

    # G2: check dep dirs exist (warn only — a missing compat may still boot)
    for dep in deps_list:
        dep_dir = deploy_dir / "mods" / dep
        if not dep_dir.is_dir():
            print(f"[runner] [ENV-GUARD] WARNING: compat dep dir missing: {dep_dir} "
                  f"for mod={e.mod!r} — mission may fail to load ABL/pak resources",
                  file=sys.stderr)

    # G3: clear stale .modindex-cache for mod + each dep
    all_mods_to_clear = [e.mod] + deps_list
    for mod_name in all_mods_to_clear:
        cache_path = deploy_dir / "mods" / mod_name / ".modindex-cache"
        try:
            if cache_path.exists():
                cache_path.unlink()
                print(f"[runner] [ENV-GUARD] cleared stale .modindex-cache for {mod_name}",
                      file=sys.stderr)
        except Exception as _exc:
            print(f"[runner] [ENV-GUARD] WARNING: could not clear .modindex-cache "
                  f"for {mod_name}: {_exc}", file=sys.stderr)

    return True, env_overrides, ""


def select_missions(entries, missions):
    """Resolve --mission names against manifest entries.

    Returns (selected, unknown): selected preserves manifest order and
    drops skip-tier entries and duplicates; unknown is the sorted list of
    requested names that matched no non-skip manifest entry.
    """
    wanted = set(missions)
    selected = []
    seen = set()
    for e in entries:
        if e.tier == "skip" or e.stem not in wanted or e.stem in seen:
            continue
        selected.append(e)
        seen.add(e.stem)
    unknown = sorted(wanted - seen)
    return selected, unknown


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["tier1", "tier2", "tier3"])
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--allow-any-mission", action="store_true",
                    help="allow --mission stems not present in "
                         "tests/smoke/smoke_missions.txt, as long as a matching "
                         "<stem>.fit or <stem>.pak exists under the resolved "
                         "deploy's data/missions dir. Prints a WARNING per "
                         "auto-accepted stem. Default (no flag) behavior is "
                         "unchanged: unknown stems still hard-error (exit 2).")
    ap.add_argument("--menu-canary", action="store_true")
    ap.add_argument("--with-menu-canary", action="store_true")
    ap.add_argument("--menu-script", default=str(DEFAULT_MENU_SCRIPT))
    ap.add_argument("--menu-settle", type=int, default=5)
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--continue", dest="cont", action="store_true", default=True)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--allow-env-drop", action="store_true",
                    help="proceed even when MC2_* vars set in the environment "
                         "cannot be passed through to mc2.exe. Default is to "
                         "REFUSE: a run that silently ignores your gate "
                         "produces a PASS for a configuration that was never "
                         "applied (DETERMINISM-RATCHET-2).")
    ap.add_argument("--baseline-update", action="store_true")
    ap.add_argument("--kill-existing", action="store_true")
    # Safety hatch: restore old global-block behavior (ANY mc2.exe, regardless
    # of deploy path, blocks the run). Default is path-aware (foreign exes are
    # ignored with an advisory).
    ap.add_argument("--block-any-mc2", action="store_true",
                    help="block on any running mc2.exe regardless of its deploy "
                         "path (legacy global-block behavior; default is "
                         "path-aware)")
    ap.add_argument("--duration", type=int)
    ap.add_argument("--lite", action="store_true",
                    help="SMOKE-LITE-1 inner-loop profile: mc2_10 (night/urban, "
                         "heaviest) + mc2_01 (day) at --duration 10 (override "
                         "with --duration). ALL gates stay on (crash, heartbeat, "
                         "shader_error, gl_error, deploy fingerprint, ...). "
                         "Reduced soak coverage: run full --tier tier1 before "
                         "milestones/landing.")
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--show-window", action="store_true",
                    help="Start mission windows visible instead of minimized; use for FPS/perf runs.")
    ap.add_argument("--exe", default=None,
                    help="explicit path to mc2.exe; skips auto-selection but still "
                         "leases the folder (use --no-lease to bypass leasing)")
    # Deploy-folder lease / checkout system (scripts/smoke_lib/deploy_lease.py).
    # --deploy <name>: request a specific folder by short name (0.4/0.4c/0.4d-rc1/
    #   0.5.0/0.5-testing). Error if that folder is busy-and-fresh.
    # --no-lease: skip the lease system entirely (no coordination, legacy behavior).
    ap.add_argument("--deploy",
                    metavar="NAME",
                    help="request a specific deploy folder by short name "
                         "(0.4 / 0.4c / 0.4d-rc1 / 0.5.0 / 0.5-testing); "
                         "error if that folder is busy and not stale")
    ap.add_argument("--no-lease", action="store_true",
                    help="skip the deploy-folder lease system entirely "
                         "(no coordination; legacy single-session behavior)")
    # Tier 1.2 (docs/testing-strategy.md): opt-in safety net that promotes
    # GL_DEBUG_SEVERITY_HIGH from a silent log to abort(). Off by default.
    ap.add_argument("--gl-debug-fatal", action="store_true",
                    help="set MC2_GL_DEBUG_FATAL=1 (abort on GL_DEBUG_SEVERITY_HIGH)")
    # Deploy-coherence preflight (reuses deploy_payload.py --verify-only; does
    # NOT reimplement hashing). --verify-only = pure preflight (no mission, no
    # lock, no artifacts), advisory unless --strict. --verify-preflight = run
    # the verify THEN proceed to the normal smoke only if it passes.
    ap.add_argument("--verify-only", action="store_true",
                    help="reconcile the deploy target against its manifest and "
                         "exit; no mission launch (advisory unless --strict)")
    ap.add_argument("--verify-preflight", action="store_true",
                    help="run --verify-only as a gate, then proceed to the "
                         "normal smoke only if verify passes (implies --strict)")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only/--verify-preflight: a hard "
                         "mismatch (stale/missing) exits 1 instead of advisory")
    # DEPLOY-LANE-GUARD-1 opt-in: shell scripts/check-deploy-target.py before
    # smoke to catch broken-tree (0.4c) / cross-lane contention / stale-exe
    # fingerprint. OFF by default (behavior unchanged). --guard = advisory
    # (log only, never changes verdict); add --guard-fatal to abort on a
    # non-OK guard verdict.
    ap.add_argument("--guard", action="store_true",
                    help="opt-in: run scripts/check-deploy-target.py against the "
                         "resolved deploy dir before smoke (advisory log only)")
    ap.add_argument("--guard-fatal", action="store_true",
                    help="with --guard: abort the smoke on a non-OK guard verdict "
                         "(broken-tree / contended / stale / fingerprint-mismatch)")
    ap.add_argument("--require-gate", action="append", default=[],
                    metavar="MC2_VAR",
                    help="after the run, assert the named MC2_* gate reached the "
                         "child and did work (reads its proof-counter from "
                         "debug_state/latest_render_state.json; exits non-zero if "
                         "counter is 0). Repeatable. Known gates: "
                         "MC2_FRAMEGRAPH_EXECUTOR, MC2_FRAMEGRAPH_DRYRUN.")
    ap.add_argument("--skip-preflight-cache", action="store_true",
                    help="SMOKE-LATENCY-WINS-1: skip the preflight result "
                         "cache and always re-run the coherence/BOM checks "
                         "(default: cache hit replays their prior output and "
                         "skips both subprocesses when the deploy manifest + "
                         "shader tree + HEAD are unchanged since the last "
                         "cached run; the staleness check always runs fresh)")
    args = ap.parse_args()
    if args.lite:
        if args.tier or args.mission:
            ap.error("--lite selects its own missions; it cannot be combined "
                     "with --tier/--mission")
        args.mission = ["mc2_10", "mc2_01"]
        if args.duration is None:
            args.duration = 10
        print("[runner] [SMOKE-LITE] mc2_10 + mc2_01 @ "
              f"{args.duration}s, all gates on. Inner-loop check only; run "
              "full --tier tier1 before milestones.", file=sys.stderr)
    if args.gl_debug_fatal:
        os.environ["MC2_GL_DEBUG_FATAL"] = "1"

    # SMOKE-LATENCY-WINS-1 win #1: --duration hard cap + quick-tier banner.
    # CLAUDE.md rule: NEVER --duration > 30 (soak/parity tier is 30s fixed).
    # 15s is a valid opt-in "does it still boot+run+not crash" inner-loop tier
    # (see .claude/SMOKE-LATENCY-1-RECON.md §4) -- all duration-independent
    # gates (crash_silent, crash_no_summary, engine_reported_fail, gl_error,
    # pool_null, asset_oob, shader_error, missing_file) and the play-heartbeat
    # freeze gate (3s threshold) are satisfied well within 15s. It intentionally
    # loses soak coverage (mid-mission combat/FX/leak/hitch observation), so it
    # must stay opt-in and never silently replace the 30s default.
    if args.duration is not None:
        if args.duration > 30:
            print(f"[runner] ERROR: --duration {args.duration} exceeds the hard "
                  f"cap of 30s. The 30s window is the soak/parity tier and must "
                  f"never be exceeded (CLAUDE.md). Use the default (30) or a "
                  f"shorter --duration for a quick inner-loop tier.",
                  file=sys.stderr)
            sys.exit(2)
        if args.duration < 30:
            print(f"[runner] [QUICK-TIER] --duration {args.duration} < 30s "
                  f"default: this run has REDUCED SOAK COVERAGE (mid-mission "
                  f"combat/FX/AI divergence, slow leaks, and late hitches get "
                  f"a shorter observation window). Crash/heartbeat/summary "
                  f"gates are still fully valid at this duration. Use for "
                  f"inner-loop iteration only -- NOT a substitute for the 30s "
                  f"soak/parity tier before landing a change.",
                  file=sys.stderr)

    # ----- Deploy-folder lease / auto-selection --------------------------
    # Must run before any code that uses args.exe (coherence check, lock, etc.)
    # so that args.exe is always a resolved, leased path before we proceed.
    #
    # Three cases:
    #   --no-lease          : skip leasing; use args.exe as-is (default exe if None)
    #   --exe GIVEN         : lease the folder it points to, skip auto-selection
    #   --deploy NAME       : select named folder, lease it (error if busy)
    #   (nothing given)     : auto-select first available preferred folder
    _lease_folder: str | None = None  # the folder we hold a lease on (None = no-lease)

    try:
        from scripts.smoke_lib import deploy_lease as _deploy_lease
        _lease_available = True
    except Exception as _le:
        print(f"[runner] [LEASE] deploy_lease import failed: {_le}; "
              f"proceeding without leasing (--no-lease behavior)", file=sys.stderr)
        _lease_available = False

    if not _lease_available or args.no_lease:
        # No-lease path: --exe is REQUIRED here (no default fallback anymore).
        if args.exe is None:
            reason = "lease system unavailable" if not _lease_available else "--no-lease set"
            print(f"[runner] ERROR: {reason} and no --exe given. "
                  "Pass --exe <path>/mc2.exe, --deploy <name>, or let the lease "
                  "auto-acquire the least-recently-used folder.", file=sys.stderr)
            sys.exit(6)
        if args.no_lease and _lease_available:
            print("[runner] [LEASE] --no-lease: skipping lease system",
                  file=sys.stderr)
    else:
        # Lease path
        _explicit_exe = args.exe  # None means "auto-select"
        # Describe what this lease is for (recorded in the owner record).
        _tier = getattr(args, "tier", None)
        _missions = getattr(args, "mission", None)
        if _tier:
            _intended_test = f"tier:{_tier}"
        elif _missions:
            _intended_test = "missions:" + ",".join(_missions)
        else:
            _intended_test = "adhoc"
        try:
            if args.deploy:
                # --deploy <name>: named folder, must exist and not be busy
                _lease_folder = _deploy_lease.auto_acquire(
                    explicit_folder=None,
                    deploy_name=args.deploy,
                    intended_test=_intended_test,
                )
                args.exe = str(
                    (Path(_lease_folder) / "mc2.exe").resolve()
                )
            elif _explicit_exe is not None:
                # --exe given: lease its folder, honor the explicit path
                _lease_folder = _deploy_lease.auto_acquire(
                    explicit_folder=_explicit_exe,
                    deploy_name=None,
                    intended_test=_intended_test,
                )
                # args.exe unchanged — caller specified exact exe
            else:
                # Auto-select: find first available preferred folder
                _lease_folder = _deploy_lease.auto_acquire(
                    explicit_folder=None,
                    deploy_name=None,
                    intended_test=_intended_test,
                )
                args.exe = str(
                    (Path(_lease_folder) / "mc2.exe").resolve()
                )
        except (_deploy_lease.LeaseError,
                _deploy_lease.NoFolderAvailable) as _lex:
            print(f"[runner] {_lex}", file=sys.stderr)
            sys.exit(6)

        # Register a release on normal exit (try/finally below also covers
        # exceptions; atexit covers os._exit / signal kills)
        import atexit as _atexit
        _atexit.register(lambda: _deploy_lease.release_lease(_lease_folder)
                         if _lease_folder else None)

    # Defensive: args.exe must be set by here (lease auto-acquire or explicit
    # --exe/--deploy). If somehow still None, fail loudly rather than silently
    # falling back to a default that doesn't exist anymore.
    if args.exe is None:
        print("[runner] ERROR: internal: args.exe is None after lease/exe resolution. "
              "This is a bug — pass --exe explicitly to work around.", file=sys.stderr)
        sys.exit(6)

    # ----- Deploy-coherence verify (preflight) ---------------------------
    # The target dir run_smoke would launch from = parent of --exe. We call
    # deploy_payload.py --verify-only against THAT dir, reusing its sha256
    # reconciliation (no hashing reimplemented here). This block runs BEFORE
    # the concurrency lock / mission setup, so --verify-only takes no lock,
    # launches nothing, and writes nothing into the smoke artifact tree.
    def _run_verify(strict: bool) -> int:
        verify_target = str(Path(args.exe).resolve().parent)
        cmd = [sys.executable, str(ROOT / "scripts" / "deploy_payload.py"),
               verify_target, "--verify-only"]
        if strict:
            cmd.append("--strict")
        print(f"[runner] [VERIFY] reconciling deploy target {verify_target} "
              f"(strict={str(strict).lower()})", file=sys.stderr)
        try:
            _v = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        except Exception as _ve:
            print(f"[runner] [VERIFY] check error: {_ve}", file=sys.stderr)
            return 2
        for _line in ((_v.stdout or "") + (_v.stderr or "")).splitlines():
            if _line.strip():
                print(f"[runner] [VERIFY] {_line}", file=sys.stderr)
        # deploy_payload.py exit codes: 0 = clean OR advisory-clean (no
        # manifest / drift-without-strict); nonzero = hard mismatch under
        # --strict or bad config (we surface as 1).
        return 0 if _v.returncode == 0 else 1

    if args.verify_only:
        if args.verify_preflight:
            ap.error("--verify-only and --verify-preflight are mutually exclusive")
        rc = _run_verify(args.strict)
        if rc == 0:
            print("[runner] [VERIFY] preflight OK (advisory clean or match)"
                  if not args.strict else "[runner] [VERIFY] preflight OK (strict)",
                  file=sys.stderr)
        else:
            print("[runner] [VERIFY] preflight reported a hard mismatch "
                  "(--strict)", file=sys.stderr)
        sys.exit(rc)

    if args.verify_preflight:
        # Strict gate: verify must pass, else abort BEFORE any lock/mission.
        # On pass, fall through to the byte-identical normal smoke path.
        rc = _run_verify(strict=True)
        if rc != 0:
            print("[runner] [VERIFY] --verify-preflight gate FAILED; not "
                  "launching smoke.", file=sys.stderr)
            sys.exit(1)
        print("[runner] [VERIFY] --verify-preflight gate PASSED; proceeding "
              "to normal smoke.", file=sys.stderr)

    # DEPLOY-LANE-GUARD-1: opt-in pre-smoke lane guard (--guard). Default OFF ->
    # this whole block is skipped and behavior is byte-identical to before. When
    # on, it shells scripts/check-deploy-target.py; advisory unless --guard-fatal.
    if args.guard:
        try:
            _gsha = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                   capture_output=True, text=True, timeout=10)
            _gexp = _gsha.stdout.strip() if _gsha.returncode == 0 else None
            _gbr = subprocess.run(["git", "-C", str(ROOT), "rev-parse",
                                   "--abbrev-ref", "HEAD"],
                                  capture_output=True, text=True, timeout=10)
            _gbranch = _gbr.stdout.strip() if _gbr.returncode == 0 else None
            _gcmd = [sys.executable,
                     str(ROOT / "scripts" / "check-deploy-target.py"),
                     str(Path(args.exe).resolve().parent),
                     "--exe-name", Path(args.exe).name]
            if _gexp:
                _gcmd += ["--expected-sha", _gexp]
            if _gbranch:
                _gcmd += ["--branch", _gbranch]
            _g = subprocess.run(_gcmd, capture_output=True, text=True, timeout=60)
            for _line in ((_g.stdout or "") + (_g.stderr or "")).splitlines():
                if _line.strip():
                    print(f"[runner] {_line}", file=sys.stderr)
            if _g.returncode != 0:
                if args.guard_fatal:
                    print("[runner] [DEPLOY_GUARD] HARD FAIL (--guard-fatal): "
                          f"guard exit {_g.returncode}; not launching smoke.",
                          file=sys.stderr)
                    sys.exit(2)
                print("[runner] [DEPLOY_GUARD] ADVISORY: non-OK verdict "
                      f"(exit {_g.returncode}); continuing (no --guard-fatal).",
                      file=sys.stderr)
        except SystemExit:
            raise
        except Exception as _ge:
            print(f"[runner] [DEPLOY_GUARD] check skipped: {_ge}", file=sys.stderr)

    # SMOKE-LATENCY-WINS-1 win #4: amortize the coherence + BOM preflights.
    # Both re-hash/re-read the full deploy manifest / shader tree every
    # invocation even though neither can change between two back-to-back
    # `run_smoke` calls unless the deploy dir or worktree HEAD actually
    # changed. Cache their (stdout, stderr, returncode) keyed on a cheap
    # fingerprint (HEAD sha + manifest mtime/size + shader dir count/max-mtime;
    # see scripts/smoke_lib/preflight_cache.py). A hit replays the prior
    # output verbatim and skips both subprocesses; a miss (or
    # --skip-preflight-cache) re-runs them and refreshes the cache. Purely an
    # advisory-tier latency win -- BOM-check's hard sys.exit(2) on FAIL still
    # fires identically whether the result came from cache or a fresh run.
    from scripts.smoke_lib import preflight_cache as _pfcache
    _pfcache_path = ARTIFACT_ROOT / ".preflight_cache.json"
    _pfcache_fp = None
    _pfcache_hit = None
    if not args.skip_preflight_cache:
        try:
            _pfcache_fp = _pfcache.compute_fingerprint(
                Path(args.exe).resolve().parent, ROOT)
            _pfcache_hit = _pfcache.check_cache_hit(_pfcache_path, _pfcache_fp)
        except Exception as _pce:
            print(f"[runner] [PREFLIGHT-CACHE] lookup skipped: {_pce}",
                  file=sys.stderr)

    if _pfcache_hit is not None:
        print("[runner] [PREFLIGHT-CACHE] HIT -- deploy manifest + shader tree "
              "+ HEAD unchanged since last cached run; replaying coherence/BOM "
              "results (pass --skip-preflight-cache to force a fresh check).",
              file=sys.stderr)
        _coh_out = _pfcache_hit.get("coherence", {})
        for _line in (_coh_out.get("text") or "").splitlines():
            if _line.strip():
                print(f"[runner] {_line}", file=sys.stderr)
        _bom_out = _pfcache_hit.get("bom", {})
        for _line in (_bom_out.get("text") or "").splitlines():
            if _line.strip():
                print(f"[runner] [BOM-CHECK] {_line}", file=sys.stderr)
        if _bom_out.get("returncode", 0) != 0:
            print("[runner] [BOM-CHECK] FATAL (cached): BOM detected in shader "
                  "source(s). Run `py -3 scripts/check_shader_bom.py --fix` "
                  "to strip.", file=sys.stderr)
            sys.exit(2)
    else:
        # Deploy-coherence advisory (scripts/check-deploy-coherence.py): detects
        # a stale deployed exe (fix built but never copied to the run dir).
        # STRICTLY advisory: never changes verdict, exit code, or gate behavior.
        _coh_text = ""
        try:
            _coh = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "check-deploy-coherence.py"),
                 str(Path(args.exe).resolve().parent), "--worktree", str(ROOT)],
                capture_output=True, text=True, timeout=30)
            _coh_text = (_coh.stdout or "") + (_coh.stderr or "")
            for _line in _coh_text.splitlines():
                if _line.strip():
                    print(f"[runner] {_line}", file=sys.stderr)
        except Exception as _e:
            print(f"[runner] [DEPLOY_COHERENCE] check skipped: {_e}", file=sys.stderr)

        # Shader BOM preflight: UTF-8 BOM in any shader = silent compile death
        # (driver reports "unexpected token '€'" with no file name). Hard-fail
        # so a BOM-infected deploy never silently passes the smoke gate.
        _bom_text = ""
        _bom_rc = 0
        try:
            _bom = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "check_shader_bom.py")],
                capture_output=True, text=True, timeout=15)
            _bom_text = (_bom.stdout or "") + (_bom.stderr or "")
            _bom_rc = _bom.returncode
            for _line in _bom_text.splitlines():
                if _line.strip():
                    print(f"[runner] [BOM-CHECK] {_line}", file=sys.stderr)
            if _bom_rc != 0:
                print("[runner] [BOM-CHECK] FATAL: BOM detected in shader source(s). "
                      "Run `py -3 scripts/check_shader_bom.py --fix` to strip.",
                      file=sys.stderr)
                sys.exit(2)
        except Exception as _e:
            print(f"[runner] [BOM-CHECK] check skipped: {_e}", file=sys.stderr)
        finally:
            # Cache even a BOM-FAIL result: a real BOM defect stays cached as a
            # fail until the shader tree's fingerprint changes (e.g. --fix
            # strips it, which changes shaders_fingerprint), so a cache HIT can
            # never mask a genuine, still-present BOM defect.
            if _pfcache_fp is not None:
                try:
                    _pfcache.save_cache(_pfcache_path, _pfcache_fp, {
                        "coherence": {"text": _coh_text},
                        "bom": {"text": _bom_text, "returncode": _bom_rc},
                    })
                except Exception as _pse:
                    print(f"[runner] [PREFLIGHT-CACHE] save skipped: {_pse}",
                          file=sys.stderr)

    # F1 unified-projection: forbid MC2_DISABLE_GOSFX=0 in smoke runs.
    # Visual regression accepted only in dev-override sessions; smoke must
    # represent shipped default state.
    if os.environ.get("MC2_DISABLE_GOSFX") == "0":
        print("[run_smoke] FATAL: MC2_DISABLE_GOSFX=0 conflicts with unified "
              "projection; smoke would record regressed visuals. Unset or set =1.",
              file=sys.stderr)
        sys.exit(2)

    # Concurrency lock: prevent two smoke runners from stepping on each other.
    # A second invocation with --kill-existing would taskkill the first run's
    # owned mc2.exe (image-name kill, not PID-specific), producing crash_silent
    # for whichever mission was in flight.  The lock is a flat file next to the
    # artifact root; open(..., 'x') is atomic on Windows (CreateFile CREATE_NEW).
    #
    # SMOKE-LATENCY-WINS-1 win #2: PER-LANE locking. The old lock was a single
    # global smoke.lock, serializing ALL smoke runs regardless of deploy
    # folder even though the lease system (scripts/smoke_lib/deploy_lease.py)
    # already isolates concurrent runs onto distinct deploy folders (0.4,
    # 0.4c, 0.4d-rc1, 0.5.0, 0.5-testing) and --kill-existing is already
    # path-scoped (_same_path_mc2 above never cross-kills a different lane's
    # exe). The lease itself would be a sufficient sole mutex EXCEPT it does
    # not protect --no-lease callers or two --exe invocations pointed at the
    # SAME folder without going through the lease at all -- so we still keep a
    # lock file, just keyed per-lane instead of one file for the whole
    # ARTIFACT_ROOT. Two runs on DIFFERENT lanes now take DIFFERENT lock files
    # and never collide; two runs on the SAME lane still serialize exactly as
    # before (same protection, narrower scope).
    #
    # Lane key: the leased folder's short name (e.g. "0.4", "0.5-testing") when
    # a lease is held; otherwise the resolved exe's parent dir name (covers
    # --no-lease / lease-unavailable, where no folder name is known but two
    # invocations pointed at the same exe path should still serialize). Pure
    # derivation lives in _lane_lock_token so it's unit-testable in isolation.
    _short_name_fn = _deploy_lease._short_name if _lease_folder is not None else (lambda f: f)
    _lane_token = _lane_lock_token(_lease_folder, args.exe, _short_name_fn)
    _LOCK_PATH = ARTIFACT_ROOT / f"smoke.{_lane_token}.lock"
    _LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        _lf = open(_LOCK_PATH, 'x')
        _lf.write(str(os.getpid()))
        _lf.flush()
        atexit.register(lambda: _LOCK_PATH.unlink(missing_ok=True))
    except FileExistsError:
        # May be stale (previous run crashed without cleanup). Check the PID.
        try:
            import csv as _csv
            _stale_pid = int(_LOCK_PATH.read_text().strip())
            _tl = subprocess.run(
                ["tasklist", "/FI", f"PID eq {_stale_pid}", "/NH", "/FO", "CSV"],
                capture_output=True, text=True).stdout
            # Robust: match the exact PID column (CSV field 1), not a substring of
            # the whole output. The old .count(str(pid)) gave false-positives on
            # PID-substring / PID-reuse, wrongly reporting a DEAD lock as alive and
            # blocking the run with exit 5 (observed 2026-06-27).
            _still_alive = any(
                len(_row) >= 2 and _row[1].strip() == str(_stale_pid)
                for _row in _csv.reader(_tl.splitlines())
            )
        except Exception:
            _still_alive = False
        if _still_alive:
            print(f"[runner] ERROR: another smoke run is already in progress "
                  f"on lane '{_lane_token}' (PID {_stale_pid}); wait for it to "
                  f"finish or remove {_LOCK_PATH} if it is stale. A different "
                  f"lane (--deploy <name> or a distinct --exe) can run "
                  f"concurrently -- this lock only serializes runs on the "
                  f"SAME lane.", file=sys.stderr)
            sys.exit(5)
        # Stale lock — re-acquire.
        _LOCK_PATH.unlink(missing_ok=True)
        _lf = open(_LOCK_PATH, 'x')
        _lf.write(str(os.getpid()))
        _lf.flush()
        atexit.register(lambda: _LOCK_PATH.unlink(missing_ok=True))

    # Existing-process safety (path-aware). Only mc2.exe launched from OUR
    # target exe path blocks the run; foreign mc2.exe from a different deploy
    # path are advised and ignored. --block-any-mc2 restores global blocking.
    _target_exe = Path(args.exe)
    _all_procs = _enum_mc2_processes()
    same_path = _same_path_mc2(_all_procs, _target_exe)
    foreign = []
    _same_pids = {p for p, _ in same_path}
    for pid, path, cmd in _all_procs:
        if pid in _same_pids:
            continue
        foreign.append((pid, path or cmd))
    for pid, path in foreign:
        print(f"[runner] ignoring foreign mc2.exe PID {pid} at {path}",
              file=sys.stderr)
    if args.block_any_mc2:
        blockers = same_path + foreign
    else:
        blockers = same_path
    if blockers:
        block_pids = [p for p, _ in blockers]
        if args.kill_existing:
            # Kill ONLY same-path PIDs even under --block-any-mc2 we still only
            # taskkill same-path here unless legacy block selected the foreign.
            kill_pids = block_pids if args.block_any_mc2 else [p for p, _ in same_path]
            print(f"[runner] killing existing mc2.exe PIDs {kill_pids}",
                  file=sys.stderr)
            _taskkill_mc2(kill_pids)
        else:
            print(f"[runner] ERROR: mc2.exe already running (PIDs {block_pids}); "
                  f"pass --kill-existing to override.", file=sys.stderr)
            sys.exit(4)

    if args.menu_canary:
        if args.tier or args.mission:
            ap.error("--menu-canary cannot be combined with --tier/--mission")
        timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
        artifact_dir = ARTIFACT_ROOT / timestamp
        artifact_dir.mkdir(parents=True, exist_ok=True)
        sys.exit(_run_menu_canary(Path(args.exe), Path(args.menu_script),
                                  artifact_dir, args.keep_logs, args.menu_settle))

    entries = manifest.parse_manifest(MANIFEST_PATH)
    if args.mission:
        selected, unknown = select_missions(entries, args.mission)
        if unknown:
            if args.allow_any_mission:
                # --allow-any-mission: accept a stem not in smoke_missions.txt
                # if <stem>.fit or <stem>.pak resolves under the deploy's
                # data/missions dir. This exists so non-tier missions (e.g.
                # freshly-baked terrain-gen output like gaea_mountain_01) can
                # be smoke-tested without adding them to the curated manifest.
                missions_dir = Path(args.exe).resolve().parent / "data" / "missions"
                still_unknown = []
                for stem in unknown:
                    fit_path = missions_dir / f"{stem}.fit"
                    pak_path = missions_dir / f"{stem}.pak"
                    if fit_path.is_file() or pak_path.is_file():
                        print(f"[runner] WARNING: --allow-any-mission auto-accepting "
                              f"'{stem}' (not in {MANIFEST_PATH}, but found at "
                              f"{fit_path if fit_path.is_file() else pak_path}). "
                              "This mission is NOT part of the curated smoke tier "
                              "and has no baseline/duration/heartbeat tuning — "
                              "treat results as advisory.", file=sys.stderr)
                        selected.append(manifest.Entry(tier="tier1", stem=stem))
                    else:
                        still_unknown.append(stem)
                unknown = still_unknown
            if unknown:
                # Config-error class (exit 2, same as argparse / GOSFX fatal):
                # a typo'd mission list must never read as PASS to a CI gate.
                print(f"[runner] ERROR: unknown mission name(s) "
                      f"{', '.join(unknown)} not found in {MANIFEST_PATH} "
                      f"(or skip-tier), and not resolvable under data/missions "
                      f"(--allow-any-mission checked).", file=sys.stderr)
                sys.exit(2)
    elif args.tier:
        selected = [e for e in entries if e.tier == args.tier]
    else:
        ap.error("--tier or --mission required")

    if not selected:
        print("[runner] ERROR: no missions selected; empty selection is a "
              "config error, not a pass.", file=sys.stderr)
        sys.exit(2)

    baseline_data = baselines.load(BASELINE_PATH)
    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    artifact_dir = ARTIFACT_ROOT / timestamp
    artifact_dir.mkdir(parents=True, exist_ok=True)

    menu_canary_rc = None
    if args.with_menu_canary:
        print("[runner] running menu canary", file=sys.stderr)
        menu_canary_rc = _run_menu_canary(Path(args.exe), Path(args.menu_script),
                                          artifact_dir, args.keep_logs, args.menu_settle)
        if menu_canary_rc != 0 and args.fail_fast:
            print("[runner] --fail-fast: stopping after menu canary failure", file=sys.stderr)
            sys.exit(menu_canary_rc)

    # Deploy-fingerprint: expected sha = HEAD of the worktree this runner
    # lives in. Computed once; compared against the exe's startup banner.
    _fp_expected_sha = None
    _fp_hard_fail = False
    # DEPLOY-FINGERPRINT-DEFAULT-ON-1 (Truth-First arc): default ON (opt-OUT via
    # MC2_SMOKE_REQUIRE_FINGERPRINT=0). A smoke that ran a stale/wrong-lane exe
    # (fingerprint mismatch) must NOT be allowed to report PASS — that is exactly
    # the "passed on the wrong binary" failure this arc exists to kill. Escape
    # hatch preserved for intentionally running a pre-built external exe.
    _fp_require = os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1") != "0"
    # SMOKE-DEPLOY-TRAP-1: no fingerprint module => no way to tell which exe we
    # smoked. Refuse to start rather than run the missions and print a verdict
    # that cannot be attributed to a binary.
    if _fingerprint is None:
        if _fp_require:
            print(
                f"\n[runner] [DEPLOY_FINGERPRINT] FATAL: the fingerprint module "
                f"could not be imported ({_fingerprint_import_error!r}), so the "
                f"smoked exe cannot be verified against this worktree HEAD.\n"
                f"[runner] Refusing to run -- a PASS here would be unattributable "
                f"to any binary (this is exactly the stale/wrong-lane exe trap).\n"
                f"[runner] Fix scripts/smoke_lib/fingerprint.py, or set "
                f"MC2_SMOKE_REQUIRE_FINGERPRINT=0 to intentionally smoke an "
                f"unverified exe.",
                file=sys.stderr)
            sys.exit(2)
        print(f"[runner] [DEPLOY_FINGERPRINT] check UNAVAILABLE "
              f"({_fingerprint_import_error!r}) and MC2_SMOKE_REQUIRE_FINGERPRINT=0 "
              f"-- exe identity is NOT verified for this run.", file=sys.stderr)
    if _fingerprint is not None:
        try:
            _fp_expected_sha = subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                text=True, stderr=subprocess.DEVNULL, timeout=15).strip()
        except Exception:
            _fp_expected_sha = None

    # SMOKE-FINGERPRINT-FULL-COVERAGE-1: the exe fingerprint only covers the exe
    # sha; a STALE shader or PDB in the deploy target (NaN frame -> fake green)
    # is invisible unless --verify-preflight is passed. Surface deployed-tree
    # drift (stale/missing shaders/PDB/exe vs .deployed_manifest.csv) on EVERY
    # run. Advisory by default (other sessions legitimately smoke partial trees);
    # MC2_SMOKE_REQUIRE_FRESH=1 gates it to a hard pre-mission abort. Reuses
    # deploy_payload.staleness_report (single-source sha256 — no reimplemented
    # hashing).
    #
    # SMOKE-LATENCY-WINS-1 win #4 scope note: this check is deliberately NOT
    # cached (unlike coherence/BOM above). It's an in-process function call
    # (no subprocess spawn), so it's cheaper per-run than the two subprocess
    # checks, and it has a live hard-fail path (MC2_SMOKE_REQUIRE_FRESH=1 ->
    # sys.exit(7)) rather than being purely advisory -- keeping it always-fresh
    # avoids any cache-staleness risk on the one check that can actually abort
    # the run.
    _fresh_require = os.environ.get("MC2_SMOKE_REQUIRE_FRESH") == "1"
    try:
        from scripts import deploy_payload as _dp
        _stale = _dp.staleness_report(str(Path(args.exe).resolve().parent))
    except Exception as _exc:  # noqa: BLE001
        _stale = None
        print(f"[DEPLOY_STALENESS] check skipped: {_exc}", file=sys.stderr)
    if _stale is not None:
        if not _stale["has_manifest"]:
            print("[DEPLOY_STALENESS] ADVISORY: no .deployed_manifest.csv — "
                  "cannot verify shader/PDB freshness", file=sys.stderr)
        elif not _stale["version_ok"]:
            print("[DEPLOY_STALENESS] ADVISORY: unrecognized manifest version",
                  file=sys.stderr)
        elif _stale["stale"] or _stale["missing"]:
            print(f"[DEPLOY_STALENESS] {'FAIL' if _fresh_require else 'WARNING'}: "
                  f"{len(_stale['stale'])} stale, {len(_stale['missing'])} missing "
                  f"vs manifest (src_commit={_stale['src_commit']})", file=sys.stderr)
            for r in (_stale["stale"] + _stale["missing"])[:20]:
                tag = "STALE" if r in _stale["stale"] else "MISSING"
                print(f"[DEPLOY_STALENESS]   {tag}: {r}", file=sys.stderr)
            if _fresh_require:
                print("[runner] MC2_SMOKE_REQUIRE_FRESH=1: deployed tree drifted "
                      "from manifest — aborting before missions", file=sys.stderr)
                sys.exit(7)
        else:
            print(f"[DEPLOY_STALENESS] OK: {_stale['ok']} files match manifest "
                  f"(src_commit={_stale['src_commit']})", file=sys.stderr)

    # VEG-SMOKE-FLOOR-1: only active when MC2_VEGETATION_CARDS=1.
    _veg_floor_active = os.environ.get("MC2_VEGETATION_CARDS") == "1"
    _veg_floor_failures: list[str] = []  # accumulated across all missions

    # META-FIX design F (ORDER-LIVENESS floor): only active when the fixture
    # gate is set in the runner env (VEG activation-flag pattern) — non-fixture
    # runs are untouched. Absence of ORDER_LIVENESS_SUMMARY is a failure.
    _order_liveness_active = (
        os.environ.get("MC2_FIXTURE_ORDER_LIVENESS") not in (None, "", "0"))
    _order_liveness_failures: list[str] = []

    rows: list[report.Row] = []
    # MC2_DIAGNOSTIC_TRACE_FILE: normalise to absolute once, outside the
    # per-mission loop.  The game launches with cwd=exe_dir (not the runner
    # cwd), so a relative path would land in the deploy dir rather than the
    # runner's working directory — the MCP tool (mc2_render_state_server.py)
    # would then be unable to find it.  Absolute paths pass through unchanged.
    _exe_dir = Path(args.exe).resolve().parent
    _trace_file_raw = os.environ.get("MC2_DIAGNOSTIC_TRACE_FILE", "")
    _trace_file_abs = (
        str(_exe_dir / _trace_file_raw)
        if _trace_file_raw and not Path(_trace_file_raw).is_absolute()
        else _trace_file_raw
    )

    for e in selected:
        # Lease heartbeat: refresh last_used timestamp at the start of each mission
        # so a multi-mission run does not go stale mid-run.
        if _lease_folder is not None and _lease_available:
            try:
                _deploy_lease.touch_lease(_lease_folder)
            except Exception:
                pass  # heartbeat failure is non-fatal

        # SMOKE-MODMISSION-ENV-GUARDS-1: pre-launch env + .fit validation for mod missions.
        _mod_ok, _mod_env, _mod_err = _modmission_env_guard(
            e, deploy_dir=Path(args.exe).resolve().parent, caller_env=os.environ)
        if not _mod_ok:
            print(_mod_err, file=sys.stderr)
            _env_miss_verdict = report.Row(
                stem=e.stem,
                verdict=_Verdict(passed=False,
                                 buckets=["env_mission_not_found"],
                                 details=[_mod_err]),
                summary=_LogSummary(),
            )
            rows.append(_env_miss_verdict)
            if args.fail_fast:
                break
            continue

        duration = args.duration or e.duration or 120
        tier = args.tier or "adhoc"
        cfg = RunConfig(
            exe=[args.exe],
            profile=e.profile or args.profile,
            stem=e.stem,
            duration=duration,
            heartbeat_timeout_load_s=e.heartbeat_timeout_load or 60,
            heartbeat_timeout_play_s=e.heartbeat_timeout_play or 3,
            grace_s=60,
            allow_asset_oob=e.allow_asset_oob,
            # META-FIX INFRA-3: per-mission marker-bucket downgrades from the
            # manifest (allow_asset_oob analog for registered marker rows).
            marker_overrides=e.marker_overrides,
            show_window=args.show_window,
            env_extra={
                "MC2_SMOKE_SEED": "0xC0FFEE",
                # META-FIX design A: force the read-only mover-registry
                # coherence audit ON for tier runs (same spirit as
                # MC2_DEBUG_STATE_DUMP in the canonical command — shipping
                # default stays OFF). Any [MOVER_COHERENCE] VIOLATION line
                # red-flags the mission via the mover_coherence marker bucket.
                **({"MC2_MOVER_COHERENCE_AUDIT": "1"} if args.tier else {}),
                # SMOKE-MODMISSION-ENV-GUARDS-1: inject MC2_ACTIVE_MOD/MC2_MOD_DEPS
                # from manifest (already validated by _modmission_env_guard above).
                **_mod_env,
                # MC2_DIAGNOSTIC_TRACE_FILE is always absolute (computed above)
                # so the engine writes to _exe_dir/debug_state/... regardless
                # of where the smoke runner is invoked from.
                **({"MC2_DIAGNOSTIC_TRACE_FILE": _trace_file_abs} if _trace_file_abs else {}),
                # Propagate PatchStream env vars from parent if set —
                # subprocess.Popen's env arg replaces the inherited env
                # entirely, so vars not explicitly listed get dropped.
                **{k: v for k, v in os.environ.items()
                   if k in ("MC2_FX_COUNT_LOG",
                            "MC2_PIPELINE_BIND_TRACE",  # SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: shadow pipeline bind trace
                            "MC2_RES_DIAG",  # [RES_DIAG v1] HUD/scene resolution-split one-shot dump
                            "MC2_TEXMGR_KTX_PRIMARY",  # route-2 CPU BC7 decode test gate
                            "MC2_TEXMGR_LOAD_TRACE",  # residency ground-truth load trace
                            # S9D deterministic fixed-timestep smoke clock
                            # (opt-in; Popen replaces env so it must be listed).
                            "MC2_SMOKE_FIXED_TIMESTEP",
                            # SIM-CLOCK-SPEED-1: sim/wall clock trace + sim-speed
                            # multiplier. Without these in the allowlist Popen
                            # drops them and gate-ON smoke runs are inert.
                            "MC2_SIM_CLOCK_TRACE",
                            "MC2_SIM_SPEED",
                            "MC2_SCREENSHOT_AT_FRAME",
                            "MC2_SCREENSHOT_PATH",
                            "MC2_OBJECT_POLY_OFFSET",
                            "MC2_OBJECT_POLY_OFFSET_FACTOR",
                            "MC2_OBJECT_POLY_OFFSET_UNITS",
                            "MC2_OBJECT_RECON_TRACY",
                            "MC2_HITCH_TRACE",
                            "MC2_HITCH_MS",
                            # [RENDER_PASS_TIME v1] coarse per-pass GPU timers
                            "MC2_RENDER_PASS_TIME",
                            "MC2_RENDER_PASS_TIME_EVERY",
                            # [FRAME_PASS_STATS v1] per-pass advisory stats
                            "MC2_FRAME_PASS_STATS",
                            "MC2_FRAME_PASS_STATS_EVERY",
                            "MC2_DEBUG_STATE_DUMP",
                            # UNITPROFILE-TYPELOAD-SOURCE-1: data-driven unit
                            # capability gate. Without this allowlisted Popen
                            # drops it and the gate-ON witness proof is inert.
                            "MC2_UNIT_PROFILE_DATA",
                            # UNIT-SOURCE-FLIP-SWITCH-1: unit-definition SOURCE
                            # selector (legacy|generated, DEFAULT legacy). Must be
                            # allowlisted or Popen drops it and the generated
                            # sourcing path never runs under the smoke gate.
                            "MC2_UNIT_DEFINITION_SOURCE",
                            # EQUIPMENT-SOURCE-FLIP-SWITCH-1: equipment-definition
                            # SOURCE selector (legacy|generated, DEFAULT legacy).
                            # Must be allowlisted or Popen drops it and the
                            # generated stat-overlay never runs under the gate.
                            "MC2_EQUIPMENT_DEFINITION_SOURCE",
                            "MC2_PRESWAP_FINISH",
                            "MC2_MODERN_TERRAIN_SURFACE",
                            "MC2_MODERN_TERRAIN_PATCHES",
                            "MC2_SHAPE_C_PARITY_CHECK",
                            "MC2_PATCH_STREAM_TRACE",
                            "MC2_PATCH_STREAM_FORCE_INIT_FAIL",
                            "MC2_PATCHSTREAM_QUAD_RECORDS",
                            "MC2_PATCHSTREAM_QUAD_RECORDS_DRAW",
                            "MC2_PATCHSTREAM_THIN_RECORDS",
                            "MC2_PATCHSTREAM_THIN_RECORDS_DRAW",
                            "MC2_PATCHSTREAM_THIN_RECORD_FASTPATH",
                            "MC2_THIN_DEBUG",
                            "MC2_WATER_DEBUG",
                            "MC2_WATER_STREAM_DEBUG",
                            "MC2_RENDER_WATER_FASTPATH",
                            "MC2_RENDER_WATER_PARITY_CHECK",
                            "MC2_VERTEX_PROJECT_FAST",
                            "MC2_VERTEX_PROJECT_PARITY",
                            "MC2_TERRAIN_INDIRECT",
                            "MC2_TERRAIN_INDIRECT_PARITY_CHECK",
                            "MC2_TERRAIN_INDIRECT_TRACE",
                            # Decal static-bake (drawPass-retirement Slice A) kill-switch
                            "MC2_TERRAIN_INDIRECT_OVERLAY",
                            # Ring-buffer hazard probe (raster-triangle bug)
                            "MC2_RING_TRACE",
                            # Probe 7: force glFinish() between compute and draw
                            "MC2_RING_FORCE_FINISH",
                            # WATER-THINRING-FENCE-1: per-slot GLsync fence trace
                            # on the water thin-record ring (diagnostics-only).
                            "MC2_WATER_THINRING_TRACE",
                            # ISSUE-42a real-salvage-path soak fixture (default
                            # OFF; pairs with MC2_SOAK_AUTOWIN/KILL_ENEMY).
                            "MC2_SOAK_SALVAGE_TRADEMOVER",
                            # ISSUE-50 link-up latch diagnostics (default OFF).
                            "MC2_LINKUP_TRACE",
                            # ISSUE-34 terrain tessellation kill-switch (default
                            # ON; =0 exercises the no-tess config headlessly).
                            "MC2_TERRAIN_TESS",
                            # DETERMINISM-RATCHET-2: letterbox-bar opacity
                            # killswitch (default ON; =0 restores the legacy
                            # alpha ramp). It was read by code/gamecam.cpp:841
                            # but absent from this allowlist, so a smoke run
                            # setting MC2_LETTERBOX_OPAQUE=0 silently ran the
                            # gate-ON path and "verified" the wrong build. That
                            # is the ENV_DROP class: the verification is void,
                            # and nothing says so.
                            "MC2_LETTERBOX_OPAQUE",
                            # DETERMINISM-RATCHET-2: per-mover state dump
                            # ([MOVER v2] lines, ~2 Hz) + its period knob. Same
                            # ENV_DROP gap; diagnostics-only, default OFF. Used
                            # to attribute run-to-run capture drift to movers.
                            "MC2_BRIDGE_MOVER_STATE",
                            "MC2_BRIDGE_MOVER_PERIOD_SEC",
                            # SIM-IDENTITY-HASH-1: the successor to the two
                            # above -- one [SIMID v1] world-hash line per SIM
                            # frame instead of ~58k per-mover lines, so
                            # `diff run_a.log run_b.log | head -1` names the
                            # first divergent sim frame. Shipped in baf64733
                            # WITHOUT these three entries, which reintroduced
                            # the exact ENV_DROP the MC2_LETTERBOX_OPAQUE
                            # comment above records: MC2_SIM_IDENTITY_TRACE=1
                            # on a smoke run was dropped by Popen, the trace
                            # never fired, and the headline claim ("localise
                            # the residual to the first divergent sim frame")
                            # was unreachable from the smoke harness.
                            # MC2_SIM_IDENTITY is default ON; the other two are
                            # default OFF (code/sim_identity.cpp:80,85,91 is the
                            # reader). Added by VERIFY-THE-UNVERIFIED-1.
                            "MC2_SIM_IDENTITY",
                            "MC2_SIM_IDENTITY_TRACE",
                            "MC2_SIM_IDENTITY_TRACE_DETAIL",
                            # ISSUE-9 called-shot gate + headless aimed-fire probe
                            # (both default OFF).
                            "MC2_CALLED_SHOT_FIX",
                            "MC2_AIMED_FIRE_FIXTURE",
                            # ISSUE-17 fire-refusal instrumentation (default OFF;
                            # nofire/firegate events on the COMBAT diag tag).
                            "MC2_FIRE_DECISION_TRACE",
                            "MC2_VK_SHADOW_MATRIX_DIAG",
                            # ISSUE-10 / SAVE-LOAD-AIFREEZE: per-tick runBrain
                            # decision-state stderr trace (default OFF).
                            "MC2_BRAIN_FREEZE_TRACE",
                            # META-FIX-BATCH-2 design D: resolution/aspect
                            # contract audit (off/log/fatal) + the window-shape
                            # overrides the resolution matrix drives. Without
                            # these in the allowlist Popen drops them and every
                            # matrix entry silently runs at the default
                            # resolution with the audit disarmed (ENV-DROP).
                            "MC2_RES_ASSERT",
                            "MC2_WIDTH",
                            "MC2_HEIGHT",
                            "MC2_WINDOWED",
                            # NOFOCUS-HARDEN-1: create the engine window hidden
                            # off the visible desktop (-32000,-32000) instead of
                            # letting a 250ms external poller move it after the
                            # fact. Grouped with the window-shape overrides above
                            # because it belongs to the same surface. Listed here
                            # for convention parity -- smoke_lib/runner.py builds
                            # the child env from os.environ.copy(), so it already
                            # reaches mc2.exe -- but if the runner ever stops
                            # inheriting os.environ, dropping THIS one silently
                            # puts a desktop-sized window back on the user's
                            # screen, which is the exact complaint that got game
                            # launches hard-blocked.
                            "MC2_WINDOW_OFFSCREEN",
                            "MC2_FORCE_43",
                            "MC2_CAMERA_ASPECT_NATIVE",
                            # META-FIX-BATCH-2 design F: order-liveness fixture
                            # (move+attack orders + ORDER_LIVENESS_SUMMARY).
                            # Also the runner-floor activation flag.
                            "MC2_FIXTURE_ORDER_LIVENESS",
                            # R2B-STATIC-NATURAL-TOUCH-PRESERVE-1: default-ON
                            # killswitch + diagnostics for the static-natural
                            # (tree/pine-building) registry stale-drop fix. Without
                            # these allowlisted, Popen drops them and the gate-OFF
                            # A/B (preserve=0) plus both trace streams are inert.
                            "MC2_R2B_TOUCH_PRESERVE",
                            "MC2_R2B_STATIC_NATURAL_TRACE",
                            "MC2_REGFLUSH_DIAG_TRACE",
                            "MC2_STATIC_STALE_DROP_FATAL",
                            # OMT-1-OVERLAY-MISSING-TEXTURE-GUARD: overlay/decal
                            # resolve-fail counters + fallback-bind trace.
                            "MC2_OVERLAY_TEXTURE_TRACE",
                            # MC2-VERIFY-LIVE-1: live data-contract guard mode
                            # (log/fatal/off; default log). Must be allowlisted
                            # or Popen drops it and the fatal-mode soak leg
                            # silently runs in default log mode.
                            "MC2_VERIFY_MODE",
                            # OVERLAY-TILE-HIRES-1 -- hi-res overlay tile loader
                            # (default OFF). Must be allowlisted or Popen drops it
                            # and the gate-ON smoke runs the legacy 64px path.
                            "MC2_OVERLAY_TILE_HIRES",
                            # TEAM-COMMANDER-OWNERSHIP-1: in-process idempotency
                            # probe. On mission teardown it re-runs the unified
                            # resetTeamsAndCommanders() (the ex-double-free) and
                            # MC2_VERIFYs it was a clean no-op with home NULL.
                            # Must be allowlisted or Popen drops it and the
                            # gate-ON cycle soak silently does nothing.
                            "MC2_MISSION_CYCLE_TEST",
                            # FRAME-CURRENTNESS-GUARDS-1 gates
                            "MC2_REGFLUSH_GUARD2",
                            "MC2_TARGETING_GUARD",
                            "MC2_ANIM_CADENCE_GUARD",
                            "MC2_ANIM_CADENCE_FIX",
                            # GPU object batcher gate (bisect partner for terrain bug)
                            "MC2_GPU_OBJECTS",
                            # Shadow-lane gated features (smoke coverage for the
                            # dynamic sun-shadow caster path + static building map).
                            "MC2_SHADOW_ENABLE",
                            # SHADOW-PROP-ALPHA-1 / SHADOW-PROP-ALPHA-ATLAS-1:
                            # prop-shadow alpha-test gates (default ON). Must be
                            # allowlisted or Popen drops them and a gate-OFF A/B
                            # smoke silently runs gate-ON.
                            "MC2_SHADOW_PROP_ALPHA",
                            "MC2_SHADOW_PROP_ALPHA_ATLAS",
                            # SHADOW-DIST-FADE-1: distance-scaled dynamic-shadow
                            # penumbra + fade dissolve distance (wu, default 80,
                            # =0 killswitch). Must be allowlisted or a gate-OFF
                            # A/B smoke silently runs the default.
                            "MC2_SHADOW_DIST_FADE",
                            # SHADOW-DONUT-FIX-1: fade opacity cap (default 0.65).
                            "MC2_SHADOW_DIST_FADE_MAX",
                            # SHADOW-FADE-CONTINUOUS-1: continuous occluder-distance
                            # estimate (default ON; =0 restores 4-step ladder).
                            "MC2_SHADOW_FADE_CONT",
                            # SHADOW-FADE-RAMP0-1: continuous-from-contact fade
                            # (default ON; =0 restores 0.45-onset window/seam).
                            "MC2_SHADOW_FADE_RAMP0",
                            # SHADOW-ALT-SCALE-1: sun-altitude-scaled penumbra/
                            # fade gains (default ON; =0 fixed low-sun gains).
                            "MC2_SHADOW_ALT_SCALE",
                            # LIGHT-CULL-RADIUS-1: light-volume frustum test +
                            # owner-intent composition (default ON).
                            "MC2_LIGHT_CULL_RADIUS",
                            # MOVER-LIGHT-FORWARD-1: mover pool forward bias wu.
                            "MC2_LIGHT_POOL_MOVER_FWD",
                            # POOL-CONSUMERS-1: object-shader pool consumption.
                            "MC2_LIGHT_POOL_OBJECTS",
                            "MC2_LIGHT_POOL_OBJ_SCALE",
                            # LIGHT-CONE-MODELS-1: authored-cone aim/spill +
                            # additive cone glow (defaults ON) + knobs.
                            "MC2_LIGHT_CONE_NODES",
                            "MC2_LIGHT_CONE_GLOW",
                            "MC2_LIGHT_CONE_GLOW_ALPHA",
                            "MC2_LIGHT_CONE_SPILL",
                            # GHOST-LIGHT-VISIBILITY-1: cone glow follows the
                            # model's own draw predicate (default ON).
                            "MC2_LIGHT_CONE_VIS_GATE",
                            "MC2_VIS_SEMANTICS",
                            "MC2_FORCE_ALL_VISIBLE",
                            "MC2_VIS_SEMANTICS_TRACE",
                            "MC2_VIS_SEMANTICS_TRACE_EVERY",
                            "MC2_LIGHT_MOVER_VIS_GATE",
                            # CONE-AIM-DIAG-1 / CONE-AIM-FLIP-1 / GHOST-LIGHT-
                            # DUMP-1 (batch-7 items 4+5 diagnostics).
                            "MC2_LIGHT_CONE_DIAG",
                            "MC2_LIGHT_CONE_AIM_FLIP",
                            "MC2_LIGHT_POOL_DUMP",
                            # LIGHT-REALISM-1: soft/feathered cones + pool
                            # continuity (default OFF) + taste knobs.
                            "MC2_LIGHT_REALISM",
                            "MC2_LIGHT_REALISM_CONE_OPACITY",
                            "MC2_LIGHT_REALISM_CONE_FEATHER",
                            "MC2_LIGHT_REALISM_POOL_SPILL",
                            "MC2_LIGHT_REALISM_POOL_CORE",
                            "MC2_LIGHT_REALISM_BEACON",
                            "MC2_LIGHT_REALISM_BEACON_RADIUS",
                            # LIGHT-POOL-CAP-1: pool slot budget.
                            "MC2_LIGHT_POOL_MAX",
                            # GHOST-CONE-RECIPE-SKIP-1: spotlight cone leaves
                            # excluded from static recipes (default ON).
                            "MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP",
                            # WATER-SHORE-MASK-1: smoothed-contour water shore feather.
                            "MC2_WATER_SHORE_MASK",
                            # SHADOW-STABILITY-1: per-pass GL-state trace gate.
                            "MC2_SHADOW_STATE_TRACE",
                            # GLSTATE-SHADOW-CLIP-RESTORE-1: post-shadow GL state restore trace.
                            "MC2_GLSTATE_TRACE",
                            # GLSTATE slice 1: SSBO slot save/restore trace.
                            "MC2_GLSTATEGUARD_LOG",
                            "MC2_STATIC_PROP_BUILDING_SHADOW",
                            # STATIC-BLDG-SHADOW-REBAKE-1: rebake-on-destroy kill-switch/trace.
                            "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE",
                            "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME",
                            "MC2_SHADOW_DYNAMIC_PROP_CASTERS",
                            "MC2_SHADOW_BOUNDED_NEAR_FIT",
                            "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
                            "MC2_SHADOW_FRUSTUM_DIAG",
                            # SHADOW-CASTER-LIGHTBOX-CULL-1 (2026-06-04): per-frame
                            # cull of dynamic prop shadow casters to the shadow
                            # frustum (default OFF). Without these in the allowlist
                            # subprocess.Popen drops them and the gate-ON smoke
                            # state is meaningless (cull never engages).
                            "MC2_SHADOW_CASTER_LIGHTBOX_CULL",
                            "MC2_SHADOW_CASTER_CULL_MARGIN",
                            "MC2_SHADOW_CULL_DEBUG",
                            # SHADOW-FOCUS-CENTER-1 (2026-06-04): center the
                            # dynamic shadow box on the camera near-ground focus
                            # point instead of the frustum-corner AABB centroid
                            # (default OFF). Popen replaces env -- without these
                            # in the allowlist the gate-ON smoke does nothing.
                            "MC2_SHADOW_FOCUS_CENTER",
                            "MC2_SHADOW_FOCUS_DIST",
                            # TERRAIN-CONTROLMAP-SAMPLE-1 — authored control-map
                            # override (default OFF). Must be allowlisted or Popen
                            # drops it and the gate-ON smoke does nothing.
                            "MC2_TERRAIN_CONTROLMAP",
                            "MC2_TERRAIN_CONTROLMAP_FILE",
                            # TERRAIN-OVERLAY-V2-PARITY-1 -- authored cement/pad/
                            # runway overlay sidecar override (default OFF). Must
                            # be allowlisted or Popen drops it and the gate-ON
                            # smoke does nothing.
                            "MC2_TERRAIN_OVERLAY_V2",
                            "MC2_TERRAIN_OVERLAY_V2_FILE",
                            # TERRAIN-DECAL-SLICE-0A -- CLIFF_WALL mesh-decal
                            # face-frame submit for cliff-wall decoration types
                            # (default OFF). Must be allowlisted or Popen drops it
                            # and the gate-ON smoke silently runs gate-OFF.
                            "MC2_TERRAIN_DECAL",
                            "MC2_TERRAIN_DECAL_TRACE",
                            # TERRAIN-DECAL-SLICE-0B -- live CLIFF_WALL mesh-decal
                            # placement knobs (default = Slice-0A byte-identical).
                            "MC2_TERRAIN_DECAL_SCALE",
                            "MC2_TERRAIN_DECAL_OFFSET",
                            "MC2_TERRAIN_DECAL_LATERAL",
                            "MC2_TERRAIN_DECAL_LIFT",
                            "MC2_TERRAIN_DECAL_YAW",
                            "MC2_TERRAIN_DECAL_PITCH",
                            # TERRAIN-DECAL-FILL-1 -- shadow-side ambient/fill
                            # floor for the cliff decal (default 0.20). Flag-gated
                            # to the MarbleCliff decal only.
                            "MC2_TERRAIN_DECAL_FILL",
                            # TERRAIN-DECAL-COLORBLEND-1 -- RVT-style terrain-colormap
                            # blend for the cliff decal (default 0.35). Flag-gated to
                            # the MarbleCliff decal only; 0.0 = byte-identical OFF.
                            "MC2_TERRAIN_DECAL_COLORBLEND",
                            # TERRAIN-SHORELINE-MASK-1 -- authored land-side wet/foam
                            # shoreline mask sidecar override (default OFF). Must be
                            # allowlisted or Popen drops it and the gate-ON smoke
                            # does nothing.
                            "MC2_TERRAIN_SHORELINE",
                            "MC2_TERRAIN_SHORELINE_FILE",
                            # SHORELINE-GLOW-1 -- opt in to the V3 band actually
                            # suppressing the legacy screen-space runShoreline()
                            # pass (default OFF). Without this passthrough the
                            # A/B below runs OFF against OFF.
                            "MC2_SHORELINE_LEGACY_SUPPRESS",
                            # SHORELINE-STAIRCASE-1 (MASTER-FAIL #1): water-edge
                            # shore-band feather floor in world units (default
                            # OFF). Widens the FS shore smoothstep + beach-ring
                            # shoreExt so the tile-quantized water outline
                            # dissolves into a soft gradient. Must be allowlisted
                            # or Popen drops it and the gate-ON smoke runs OFF.
                            "MC2_WATER_SHORE_FEATHER",
                            "MC2_WATER_SHORE_MEANDER",
                            "MC2_WATER_SHORE_MEANDER_FREQ",
                            "MC2_WATER_SHORE3_DEPTH_K",
                            "MC2_WATER_SHORE3_EXTRA_WU",
                            # WATER-SUN-SPEC-2 -- Blinn-Phong sun glint on the
                            # water plane (default OFF, value is an intensity).
                            # Its predecessor WATER-SUN-SPEC-1 shipped with the
                            # shader half reverted and the uniform never
                            # declared, so it was a no-op at every value; this
                            # passthrough is what lets a gate-ON smoke prove the
                            # repair actually reaches the shader.
                            "MC2_WATER_SHINE",
                            # EDGE-LIGHT-BILLBOARD-CULL-1 (MASTER-FAIL #8):
                            # render beacon/runway edge-light glow cards
                            # two-sided so they don't backface-cull toward
                            # top-down (killswitch, default ON). Must be
                            # allowlisted or Popen drops it and gate-ON runs OFF.
                            "MC2_LIGHT_BILLBOARD_TWOSIDED",
                            # LIGHT-BILLBOARD-GPU-1 -- camera-facing billboard
                            # for the same glow cards on the GPU static-prop
                            # batcher path (killswitch, default ON; =2
                            # exaggerates for verification).
                            "MC2_LIGHT_BILLBOARD_FACING",
                            # NIGHT-LIGHT-POOLS-2/3/4 -- pool falloff/dim/compress
                            # killswitches + mover radius/max-blend (defaults
                            # ON/0.25/ON/0.6/ON).
                            "MC2_LIGHT_POOL_SOFT",
                            "MC2_LIGHT_POOL_MOVER_DIM",
                            "MC2_LIGHT_POOL_COMPRESS",
                            "MC2_LIGHT_POOL_MOVER_RADIUS",
                            "MC2_LIGHT_POOL_MOVER_MAXBLEND",
                            # LIGHT-CLUSTER-PERF-1 -- world-space XY light grid
                            # over the pool (killswitch, default ON) + its cells
                            # -per-axis knob. Must be allowlisted or Popen drops
                            # them and a MC2_LIGHT_GRID=0 A/B silently runs ON.
                            "MC2_LIGHT_GRID",
                            "MC2_LIGHT_GRID_DIM",
                            # CAMERA-PARK-1: one-shot pose pin. Every pose-pinned
                            # smoke (perf arms, A/B captures) needs this to reach
                            # the child -- without it the runner warns ENV-DROP and
                            # the run silently measures the DEFAULT camera instead
                            # of the pose you asked for.
                            "MC2_CAMERA_PARK",
                            # UI-COMMAND-CHANNEL-1 / GAMEPLAY-DRIVER-1/2: the
                            # command-file verb channel and its test-spawn arm.
                            # Without these on the allowlist Popen drops them and
                            # a scenario-driven smoke silently runs with NO
                            # command channel at all -- every verb is a no-op and
                            # the run still reports success.
                            "MC2_UI_COMMAND_FILE",
                            "MC2_TEST_SPAWN",
                            # FREEZE-MOVERS-1: =<frame> pins every mover at a
                            # deterministic mission frame so a pose framing a
                            # vehicle/mech is byte-comparable across BUILDS.
                            # Must be allowlisted or Popen drops it and a
                            # "frozen" arm silently runs a LIVE sim -- which is
                            # exactly the failure that produced a retracted
                            # +14.8 luma result (a turret at a different aim
                            # angle read as a lighting change).
                            "MC2_FREEZE_MOVERS_AT",
                            # CONE-SPILL-GRADE-1: isolates the ungated
                            # c1f98b45 cone/terrain spill coupling so the
                            # OFF arm of an A/B capture actually reaches the
                            # child instead of silently running the default.
                            "MC2_CONE_SPILL_COUPLING",
                            # CONE-SPILL-POLICY-1: default-path pool policy
                            # (0=legacy unconditional, 1=up-reject + extended
                            # -reach intersection) + reach multiplier. Must
                            # reach the child so a policy A/B is real.
                            "MC2_CONE_POOL_POLICY",
                            "MC2_CONE_POOL_REACH",
                            # SPOTLIGHT-DIRECTION-1: building spotlight
                            # direction/lightToWorld coordinate-space fix. Must
                            # reach the child or the ON arm of an A/B night
                            # capture silently measures the default.
                            "MC2_SPOTLIGHT_DIRECTION_FIX",
                            # SPOTLIGHT-DIRECTION-1 measurement instrument --
                            # allowlisted so a capture arm can prove it actually
                            # reached the TG_LIGHT_POINT/SPOT consumer.
                            "MC2_FULLBAKE_DIAG",
                            # MC2_DIAG_VERBOSE: unmasks MC2_DIAG* receipts (e.g.
                            # [LIGHT_GRID v1], [LIGHT_POOL_CAP v1]) that the
                            # quiet-default build suppresses.
                            "MC2_DIAG_VERBOSE",
                            # LIGHT-POOL-CAP-1 -- terrain light pool slot count
                            # (default 256, raised from 64). Allowlisted so a
                            # cap=64 baseline arm is actually reproducible.
                            "MC2_LIGHT_POOL_MAX",
                            # MOVER-CONTACT-SHADOW-1 (MASTER-FAIL #7): under-mech
                            # night contact shadow strength (default 0 = off).
                            "MC2_MOVER_CONTACT_SHADOW",
                            # MOVER-LIGHT-HALOGEN-1: mover light color RRGGBB hex.
                            "MC2_LIGHT_POOL_MOVER_COLOR",
                            # PAD-AREA-GLOW-1: night pad-area lift (strength + spill wu).
                            "MC2_PAD_AREA_GLOW",
                            "MC2_PAD_AREA_GLOW_SPILL",
                            # RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6): solid
                            # cement pad/runway edge feather killswitch
                            # (default OFF). Must be allowlisted or Popen
                            # drops it and a gate-ON smoke silently runs OFF.
                            "MC2_RUNWAY_EDGE_BLEND",
                            # RUNWAY-APRON-TONE-2: cross-map absolute runway/
                            # apron tone normalisation + its two tunables.
                            # RUNWAY-APRON-TONE-2: cement lighting participation.
                            "MC2_CEMENT_LIT",
                            "MC2_CEMENT_RAMP_WORD",
                            # CEMENT-ATLAS-CELL-2: the two concrete-only arms
                            # (A = tone-normalise the concrete family,
                            #  B = collapse it onto one canonical cell).
                            "MC2_CEMENT_CONCRETE_TONE",
                            "MC2_CEMENT_CONCRETE_CANON",
                            # CEMENT-INTRACELL-1: ARM C = detect the baked road
                            # tileset inside solid cement cells and keep both
                            # arms off it. Also the atlas dump diagnostic.
                            "MC2_CEMENT_ROAD_ART_DETECT",
                            # CEMENT-PATCHY-CAUSE-1: give the overlay/decal
                            # layer the same concrete static-shadow exemption
                            # terrain_lod_chunk.frag applies, so the two
                            # shaders stop disagreeing across their seam.
                            "MC2_OVERLAY_CEMENT_SHADOW_MATCH",
                            # OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1): the
                            # delivered cure -- the overlay adopts the terrain's
                            # concrete albedo + lighting model on cement tiles
                            # (incl. skipping its double-applied in-shader fog).
                            # DEFAULT ON; listed so an explicit =0 revert run is
                            # not silently dropped by the ENV passthrough guard.
                            "MC2_OVERLAY_CEMENT_ADOPT",
                            # TERRAIN-AMBIENT-FLOOR-2: terrain-only ambient floor.
                            # DEFAULT ON at 0.35; listed so the =0 revert arm (the
                            # single, complete revert) is not silently dropped by
                            # the ENV passthrough guard -- an A/B where the "off"
                            # run is secretly the "on" build is how this whole
                            # family of measurements goes wrong.
                            "MC2_TERRAIN_AMBIENT_FLOOR",
                            # TERRAIN-AMBIENT-FLOOR-NIGHT-1: default-OFF floor
                            # under that daylight gate. Its whole purpose is an
                            # A/B on the five maps the day gate zeroes, so the
                            # "on" arm being silently dropped here would make the
                            # measurement report a no-op that is not real.
                            "MC2_TERRAIN_AMBIENT_FLOOR_NIGHT",
                            "MC2_TERRAIN_AMBIENT_FLOOR_TRACE",
                            "MC2_CEMENT_ATLAS_DUMP",
                            "MC2_CEMENT_ATLAS_MAGENTA_KEY",
                            "MC2_CEMENT_TRANSITION_FALLBACK",
                            # CEMENT-ALPHA-LAND-1: alpha cement slots admitted
                            # to the catalog atlas; closes the transition-tile
                            # pavement drop (mc2_10 87%, mc2_24 34%).
                            "MC2_CEMENT_ALPHA_ATLAS",
                            # PAVED-OVERLAY-CLASS-1: classify overlay-only
                            # pavement (roads/runways painted as &Overlays ids
                            # over a non-cement base slot) so it gets cement's
                            # lighting exemption. Must be allowlisted or Popen
                            # drops it and a gate-ON run silently measures OFF.
                            "MC2_PAVED_OVERLAY_CLASS",
                            # RUNWAY-CLOSE-1: overlay-art flat-light exemption
                            # A/B arm (default 1 = stock multiply).
                            "MC2_OVERLAY_ART_FLATLIGHT",
                            # TERRAIN-MATERIAL-LIB-1 — data-defined terrain material
                            # tuning (default OFF). Must be allowlisted or Popen
                            # drops it and the gate-ON smoke does nothing.
                            "MC2_TERRAIN_MATERIAL_LIB",
                            "MC2_TERRAIN_MATERIAL_LIB_FILE",
                            # TERRAIN-CONTROLMAP-ALBEDO-1 -- lifts the control-map
                            # weight-composed tint toward full albedo (default OFF).
                            # Must be allowlisted or Popen drops it and the gate-ON
                            # smoke does nothing.
                            "MC2_TERRAIN_CONTROLMAP_ALBEDO",
                            "MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH",
                            # TERRAIN-MATERIAL-TEXTURES-1 -- per-layer BC7 sRGB
                            # albedo array on the LOD-chunk terrain (default OFF)
                            # + mix-strength knob. Must be allowlisted or Popen
                            # drops them and the gate-ON smoke does nothing.
                            "MC2_TERRAIN_MATERIAL_TEXTURES",
                            "MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH",
                            # TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1 -- world-axis
                            # triplanar rock NORMAL + relief on steep cells
                            # (default OFF) + tilt-strength knob. Decoupled from
                            # the albedo gate; samples the always-loaded terrain
                            # normal array layer 5 (marble_cliff). Must be
                            # allowlisted or Popen drops them and the gate-ON
                            # capture does nothing.
                            "MC2_TERRAIN_CLIFF_TRIPLANAR",
                            "MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH",
                            # TERRAIN-CLIFF-HEIGHT-NORMAL-1 -- derive cliff shading
                            # normal from the displacement height gradient (mat5
                            # layer-5 alpha) so rock relief catches light. Tuning
                            # knob (default 2.0). Must be allowlisted or Popen drops
                            # it and the tuning capture does nothing.
                            "MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH",
                            # TERRAIN-CLIFF-POM-1 -- triplanar Parallax Occlusion
                            # Mapping on cliff faces (real marble_cliff disp in
                            # mat5 alpha), gated on top of CLIFF_TRIPLANAR. Default
                            # OFF; must be allowlisted or Popen drops them and the
                            # gate-ON capture does nothing.
                            "MC2_TERRAIN_CLIFF_POM",
                            "MC2_TERRAIN_CLIFF_POM_DEPTH",
                            "MC2_TERRAIN_CLIFF_POM_STEPS",
                            # TERRAIN-CLIFF-DEBUG -- bounded cliff material debug-viz
                            # (u_cliffDebug 1..4: cliffBlend / branch-taken / POM
                            # offset / mat5 alpha). Default 0 (off). Must be
                            # allowlisted or Popen drops it and the debug capture
                            # does nothing.
                            "MC2_TERRAIN_CLIFF_DEBUG",
                            # TERRAIN-MATERIAL-TEXTURES-1-FIX (fix A): the LOD-chunk
                            # diag bitmask drives the u_diag debug views, incl. bit
                            # 16384 = the INDEX ORACLE (each albedo array LAYER as a
                            # flat colour by array index). Allowlisted so the oracle
                            # capture reaches mc2.exe; 0/unset = no visual change.
                            "MC2_TERRAIN_LOD_CHUNK_DIAG",
                            # TERRAIN-CHUNK-POM-1 -- real view-vector POM on the
                            # LOD-chunk terrain (default OFF) + tuning knobs.
                            # Must be allowlisted or Popen drops them and the
                            # gate-ON smoke does nothing.
                            "MC2_TERRAIN_POM",
                            "MC2_TERRAIN_POM_SCALE",
                            "MC2_TERRAIN_POM_STEPS",
                            "MC2_TERRAIN_POM_NEAR",
                            "MC2_TERRAIN_POM_FAR",
                            # GROUND-CONTACT-BLOB-1 -- per-mover contact-darkening
                            # disc under mechs/vehicles (default OFF). Must be
                            # allowlisted or Popen drops it and the gate-ON smoke
                            # does nothing.
                            "MC2_GROUND_CONTACT_BLOB",
                            # PROP-SHADOW-RECEIVE-1 -- static props receive the
                            # dynamic CSM cascade (self/prop-on-prop shadow) in
                            # shadow_screen.frag (default OFF). Must be allowlisted
                            # or Popen drops it and the gate-ON smoke does nothing.
                            "MC2_PROP_SHADOW_RECEIVE",
                            # FOLIAGE-ALPHAFLAG-1 -- recipe-path per-instance
                            # flags bit 2 carries kFlagIsAlphaTest (from
                            # alphaTestOn) instead of the retired spotlight
                            # meaning (default OFF). Must be allowlisted or
                            # Popen drops it and the gate-ON smoke does nothing.
                            "MC2_PROP_FOLIAGE_ALPHAFLAG",
                            # LIGHT-POS-COLUMN-1 -- deliver the object-path
                            # light world position to the GPU (copy affine
                            # translation from floats [3]/[7]/[11] into the
                            # [12]/[13]/[14] slots every shader decodes as
                            # m[3].xyz), default OFF. Must be allowlisted or
                            # Popen drops it and the gate-ON smoke silently
                            # runs gate-OFF.
                            "MC2_LIGHT_POS_COLUMN_FIX",
                            # LIGHT-ROW-PURE-1 -- object light-ROW PRODUCTION
                            # delegated to the pure object_light_row::
                            # BuildObjectLightRow, default OFF (killswitch
                            # inverted: =1 opts in). Must be allowlisted or
                            # Popen drops it and the gate-ON verification smoke
                            # silently runs gate-OFF -- which is the whole
                            # evidence this gate is waiting on.
                            "MC2_LIGHT_ROW_PURE",
                            # LIGHT-ROW-PURE-1 -- runs BOTH row producers on the
                            # same frame and memcmps them. Pure observation,
                            # default OFF. Allowlisted for the same reason.
                            "MC2_LIGHT_ROW_PURE_VERIFY",
                            # LIGHT-POS-COMBAT-1 -- session-long per-light-type
                            # census of the uploaded light rows (source
                            # translation [3]/[7]/[11] vs uploaded position
                            # [12]/[13]/[14]). Pure observation, default OFF.
                            # Allowlisted for the same reason as the gate above.
                            "MC2_LIGHT_ROW_CENSUS",
                            # PROP-BASE-LIGHT-DIR-1 -- directional/hemispherical
                            # orientation term for the baked static-prop
                            # base_light seed (float strength 0..1, default 0.0
                            # = OFF). Must be allowlisted or Popen drops it and
                            # the gate-ON smoke silently runs gate-OFF.
                            "MC2_PROP_BASE_LIGHT_DIR",
                            # SHADOW-SUNCLAMP-1 -- back-face guard dots the same
                            # altitude-clamped sun the shadow matrices use
                            # (default ON; =0 reverts to the raw-sun guard).
                            "MC2_SHADOW_GUARD_CLAMPED_SUN",
                            # TERRAIN-SHADOW-GUARD-1 -- the terrain-side twin:
                            # shadow.hglsl's back-face guard dots the same
                            # altitude-clamped sun the shadow matrices use
                            # (default ON; =0 reverts to the raw-sun guard).
                            "MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN",
                            # SHADOW-SUN-ALT-CLAMP-1 tunable -- min effective sun
                            # altitude (deg) for the shadow matrices AND (since
                            # SHADOW-SUNCLAMP-1) the back-face guard. Default 15,
                            # 0 disables. Was ENV_DROP_RISK: read by code but not
                            # allowlisted, so a gate-ON smoke silently ran default.
                            "MC2_SHADOW_MIN_SUN_ALT",
                            # Tree/static-prop impostor LOD switch distance (WU),
                            # default 800. Same ENV_DROP_RISK fix as above.
                            "MC2_IMPOSTOR_DIST",
                            # Mask-dispatch (pre-bake-terrain merge)
                            "MC2_TERRAIN_MASK_DISPATCH",
                            "MC2_TERRAIN_MASK_DISPATCH_PARITY",
                            "MC2_TERRAIN_INDIRECT_MINE",
                            "MC2_TERRAIN_INDIRECT_OVERLAY",
                            "MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK",
                            "MC2_TERRAIN_COST_SPLIT",
                            "MC2_FX_COST_SPLIT",
                            # GPU-GROUND-PICK-PARITY-1 (S1): GPU-vs-CPU ground-pick
                            # oracle (default OFF). Requires MC2_OBJECT_ID_BUFFER=1
                            # for a live depth+objectID attachment. Without these in
                            # the allowlist Popen drops them and the [GPU_PICK_PARITY]
                            # oracle is inert (no delta/timing/fallback data emitted).
                            "MC2_GPU_GROUND_PICK_PARITY",
                            "MC2_GPU_GROUND_PICK_PARITY_SAMPLE",
                            "MC2_GPU_GROUND_PICK_PARITY_TAG",
                            "MC2_OBJECT_ID_BUFFER",
                            # FX-FORCE-SPAWN fixture (code/warrior.cpp): forces up
                            # to 8 mechs to fire all weapons (PPC -> tube ribbons)
                            # so the otherwise-idle tier1 fly-throughs exercise the
                            # tube/particle FX path. MC2_LOG=1 surfaces its stdout
                            # confirmation. Both must be allowlisted or Popen drops
                            # them and the fixture never fires.
                            "MC2_FX_FORCE_SPAWN",
                            "MC2_LOG",
                            # FX-DEFS-SIDECAR-1: EffectDef JSON overlay master gate
                            # (mclib/fx_def_registry.{h,cpp}, code/mechcmd2.cpp).
                            # Must be allowlisted or Popen drops it and the gate-ON
                            # smoke pass silently re-runs the gate-OFF path.
                            "MC2_FX_DEFS",
                            # BUG-A pick regression fence: forward so the [VK_PICK_PROBE]
                            # deterministic mouse->pick proof fires under smoke/probe runs.
                            "MC2_VK_PICK_PROBE",
                            # ABL-BAD-NATIVE-ARG-REPRO-1 gates — must be allowlisted so
                            # the repro hook and guard fire when set by the caller.
                            "MC2_ABL_ARG_GUARD",
                            "MC2_ABL_COMPILE_FATAL",
                            "MC2_ABL_COMPILE_FATAL_REPRO",
                            "MC2_ABL_SKIP_ERRORED_MODULES",
                            "MC2_ABL_RUNTIME_SOFTFAIL",
                            "MC2_ABL_ARG_GUARD_REPRO",
                            # MCO magic* AI behavior trace (code/ablmc2.cpp magicAiLog):
                            # logs whether ClanEagle brains call magicPatrol/Guard/Escort
                            # and what target the engine acquired. Allowlist or Popen drops it.
                            "MC2_MAGIC_AI_LOG",
                            # Logistics/purchase-screen load trace (code/logisticsdata.cpp
                            # updateAvailability): logs resolved purchase file path, open
                            # OK/FAIL, each Mech read, available counts. For POAR bay debug.
                            "MC2_LOG_LOGISTICS",
                            # Direct-fire projectile speed multiplier (PPC/AC/gauss);
                            # default 1.0 (stock). Allowlisted so the fixture can A/B it.
                            "MC2_PROJECTILE_SPEED_MULT",
                            # Direct-fire de-curve (straight flight + spawn lead),
                            # default-off. Allowlisted for the fixture A/B.
                            "MC2_DIRECT_FIRE_STRAIGHT",
                            # Active mod id — drives model_override + anim_override
                            # registry mod-merge (mods/<id>/...). Allowlisted so mod
                            # smokes actually load the mod; without it Popen drops it.
                            "MC2_ACTIVE_MOD",
                            # Launcher-supplied compatibility layer(s) (file.cpp MC2_MOD_DEPS):
                            # e.g. mco-compat / mc2x-compat. Required to replicate a launcher
                            # campaign launch in smoke (loads corebrain.abx + object2.pak).
                            "MC2_MOD_DEPS",
                            # META-FIX design C: [SPLIT_PATH_PARITY] runtime
                            # parity trace (gosRenderer::init). Must be
                            # allowlisted or Popen drops it and the nightly
                            # TESS=0/1 A/B leg silently runs gate-OFF.
                            "MC2_SPLIT_PATH_PARITY_TRACE",
                            # META-FIX design A: mover-registry coherence
                            # audit (code/mission.cpp). Force-set for --tier
                            # runs via env_extra above; allowlisted here so
                            # operator-set gate-ON single-mission runs also
                            # reach the child (Popen drops unlisted vars).
                            "MC2_MOVER_COHERENCE_AUDIT",
                            # META-FIX design B PART 0: opt-in file_missing
                            # trace at the msl.cpp loader chokepoint
                            # (parse_fail/required_missing are always-on and
                            # need no gate). Without this row the gate-ON
                            # diagnostic leg silently traces nothing.
                            "MC2_ASSET_TRACE",
                            # META-FIX design B PART 1: default-ON fail-loud
                            # killswitch (=0 restores exact legacy fail-soft).
                            # Allowlisted so the sabotage/probe legs can flip
                            # it through the runner.
                            "MC2_ASSET_FAILLOUD",
                            # META-FIX design B PART 3: opt-in magenta tile
                            # substitution at the icon blits (icon_oob line
                            # itself is always-on). Allowlisted for probes.
                            "MC2_ICON_FAILLOUD",
                            # META-FIX design B PART 4: observational
                            # mission_ready appearance scan (default OFF;
                            # NOT force-set for tier runs until the 24-map
                            # failed=0 baseline passes). Allowlisted so the
                            # baseline legs reach the child.
                            "MC2_APPEAR_AUDIT",
                            # META-FIX design E tier 3: file-resolve ladder
                            # diagnostics + JSONL MISS trace (mclib/file.cpp).
                            # check-file-resolve-miss.py sets both through
                            # this runner — without the allowlist rows the
                            # nightly MISS audit silently traces nothing.
                            "MC2_LOG_FILE_RESOLVE",
                            "MC2_RESOLVE_TRACE_FILE",
                            "MC2_LIGHT_COST_SPLIT",
                            "MC2_TOBJ_COST_SPLIT",
                            "MC2_STATIC_PROP_FLUSH_COST_SPLIT",
                            # GOM-UPDATE-COST-1 / TXMMGR-PERF (framebudget lane):
                            # coarse per-phase CPU cost-split emitters + perf gates.
                            # Without these allowlisted, Popen drops them and the
                            # gate-ON measurement smoke is inert (no [*_COST v1]).
                            "MC2_GOM_UPDATE_COST_SPLIT",
                            "MC2_RENDERLISTS_COST_SPLIT",
                            "MC2_LIGHT_PREFIX_GPU_COPY",
                            "MC2_SHADOW_CASTER_CULL_CACHE",
                            # SP-BATCHER-ALPHASCAN-GATE-1: skip the frame-constant
                            # per-packet alpha-test re-resolve scan when no
                            # destruction/damage-alpha event occurred since last flush.
                            "MC2_SP_ALPHASCAN_GATE",
                            # BUILDING-GLASS-1: dither-glass translucency killswitch.
                            "MC2_SP_GLASS_DITHER",
                            # LIGHTBAKE-REBAKE-ON-READY-1 killswitch.
                            "MC2_LIGHTBAKE_REBAKE_ON_READY",
                            # FOGLIGHT-LATCH-1: per-actor light-latch diagnostic.
                            "MC2_PROP_LIGHT_LATCH_DIAG",
                            "MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL",
                            "MC2_STATIC_PROP_FLUSH_CACHED_BLOB",
                            "MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
                            "MC2_STATIC_PROP_COLORS_FILL",
                            "MC2_STATIC_PROP_PERSISTENT_BUCKETS",
                            "MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
                            "MC2_BUCKET_ORDER_TRACE",
                            # GPU-cull ownership-port Slice A: cut-off upper-bound oracle
                            "MC2_GPU_CULL_OWNERSHIP_PARITY",
                            # Task 7 — superset-parity counter probe (proof-gate #2)
                            "MC2_TOBJ_PARITY",
                            # alpha-Stage 1 §5 Stage 0 — candidate-predicate
                            # disagreement probe for inView unconflation.
                            "MC2_INVIEW_CONFLATION_TRACE",
                            "MC2_GPUPROPS_TRACE",
                            # TREE-DIAG-REGISTER-1 (GATE-PARITY-CLOSE-1): the
                            # "black billboard square" diagnostic in
                            # gos_static_prop_batcher.cpp:115. PRESENCE-gated
                            # (`getenv(...) != nullptr`), so MC2_TREE_DIAG_TRACE=0
                            # ENABLES it -- unset is the only OFF. Without this
                            # line Popen drops the var and a smoke run asked to
                            # turn the diagnostic ON silently produces no
                            # [TREE_DIAG] lines at all, which reads as "the bug
                            # did not reproduce" rather than "the trace never ran".
                            "MC2_TREE_DIAG_TRACE",
                            "MC2_MECH_RESTORE_TRACE",
                            # Phase 1 — terrain lighting GPU compute
                            "MC2_TERRAIN_LIGHTING_GPU",
                            "MC2_TERRAIN_LIGHTING_PARITY",
                            # Track A1 — object admission predicate + trace
                            "MC2_OBJECT_ADMISSION_PREDICATE",
                            "MC2_PROJECTZ_TRACE",
                            "MC2_PROJECTZ_HEATMAP",
                            "MC2_PROJECTZ_SUMMARY",
                            "MC2_PROJECTZ_GUARD_PX",
                            # Task 7 — cascade-safety DESTROY trace
                            "MC2_DESTROY_TRACE",
                            # Track C0/C1a — GPU cull substrate + AABB parity + compute dispatch
                            "MC2_GPU_CULL_SUBSTRATE",
                            "MC2_GPU_CULL_AABB_PARITY",
                            "MC2_GPU_CULL_SUBSTRATE_TRACE",
                            "MC2_GPU_CULL",
                            "MC2_GPU_CULL_COMPUTE_TRACE",
                            "MC2_CAMERA_MOVE_DIAG",
                            # CAMERA-SMOOTHING-1: gameplay camera smoothing knob
                            # + evidence tooling (trace CSV, synthetic scroll driver)
                            "MC2_CAMERA_SMOOTH",
                            "MC2_CAMERA_SMOOTH_TRACE",
                            "MC2_CAM_SMOOTH_TEST",
                            "MC2_STATIC_FORCE_ADMIT",
                            "MC2_WATER_GATE_DIAG",
                            "MC2_MISSION_INTRO_LEGACY_RENDER",
                            # Track C2 — async readback ring buffer
                            "MC2_GPU_CULL_READBACK",
                            "MC2_GPU_CULL_FORCE_FENCE_NOT_READY",
                            "MC2_GPU_CULL_READBACK_TRACE",
                            # Track C3 — lifecycle gates (mech3d/gvactor/objmgr routing)
                            # m-4: missing permutation: LIFECYCLE=1+READBACK=1 and
                            # LIFECYCLE=1+READBACK=0 (fail-open regression) smoke runs.
                            # Both require --with-env overrides; not automated yet.
                            "MC2_GPU_CULL_LIFECYCLE",
                            "MC2_GPU_CULL_LIFECYCLE_TRACE",
                            # MF3-GENERATIONAL-HANDLE-1: generational watch-id
                            # validation gate (default-OFF). Allowlisted so the
                            # gate-ON smoke exercises the side-array bump/alloc path.
                            "MC2_WATCHID_GENERATION",
                            # Phase C — GPU-driven unified path
                            "MC2_GPU_DRIVEN",
                            "MC2_GPU_DRIVEN_WATER",
                            "MC2_GPU_DRIVEN_TERRAIN_SOLID",
                            "MC2_GPU_DRIVEN_OVERLAY",
                            "MC2_GPU_DRIVEN_PARITY",
                            "MC2_GPU_DRIVEN_TRACE",
                            # Reverse-Z depth verification probes (2026-05-18):
                            # depth-transition zoom-pop, water render/depth
                            # parity, reverse-Z proj-matrix lifecycle trace.
                            "MC2_DEPTH_TRANSITION_PROBE",
                            "MC2_WATER_RENDERPROBE",
                            "MC2_WATER_DEPTHPROBE",
                            "MC2_REVERSE_Z_TRACE",
                            # Terrain continuous-surface producer (2026-05-18):
                            # MC2_TERRAIN_SURFACE = path-select kill-switch
                            # (PR-1+, default-OFF); MC2_TERRAIN_SURFACE_TRACE =
                            # the PR-0 [TERRAIN_SURFACE v1] lifecycle trace gate
                            # (trace-only, default-OFF). Without these in the
                            # allowlist subprocess.Popen drops them and the
                            # forced-ON / trace smoke states are meaningless.
                            "MC2_TERRAIN_SURFACE",
                            "MC2_TERRAIN_SURFACE_TRACE",
                            # quadSetupTextures-retirement recon (2026-05-19):
                            # MC2_WATER_INVPROJ_PARITY = the [WATER_INVPROJ v1]
                            # snapshot-A-vs-B 6-tuple parity probe (terrain.cpp;
                            # trace-only, default-OFF). Decides whether the
                            # setupTextures water block contributes unique
                            # extrema beyond slimReduce. Without this in the
                            # allowlist subprocess.Popen drops it and the probe
                            # never fires.
                            "MC2_WATER_INVPROJ_PARITY",
                            # F3 CPU-projection cost-split (2026-05-20):
                            # MC2_CPU_PROJ_COST_SPLIT = master env gate for the
                            # measurement-only bucket/sidecar instrumentation
                            # (mclib/cpu_proj_cost_split.{h,cpp}). Without this
                            # in the allowlist subprocess.Popen drops it and
                            # tier1 captures emit "[CPU_PROJ v1 disabled]".
                            "MC2_CPU_PROJ_COST_SPLIT",
                            # Tier 1.2 — KHR_debug fatal-on-HIGH opt-in
                            # (docs/testing-strategy.md). Forwarded so
                            # --gl-debug-fatal reaches the engine subprocess.
                            "MC2_GL_DEBUG_FATAL",
                            # RENDER-BACKEND-IFACE / GPU-DEBUG-NAMES: render
                            # backend interface seam + GPU object labeling gates.
                            # MC2_GL_DEBUG must be forwarded so glObjectLabel is
                            # available when exercising debug names. Without these
                            # in the allowlist subprocess.Popen drops them and the
                            # gate-ON smoke runs are inert.
                            "MC2_GL_DEBUG",
                            # Perf A/B: let smoke runs explicitly disable the
                            # frame cap and vsync. Defaults stay unchanged when
                            # unset; minimized smoke FPS remains non-throughput.
                            "MC2_FPS_CAP",
                            "MC2_VSYNC",
                            "MC2_RENDER_BACKEND_IFACE",
                            # VK-P13-SELECTOR-1 / T1: top-level GL-vs-Vulkan
                            # backend selector (gl|vulkan). Must be allowlisted
                            # or Popen drops it and the =vulkan smoke silently
                            # runs on GL. (MC2_VULKAN_VALIDATION is already
                            # allowlisted below for the device-birth proof.)
                            "MC2_RENDER_BACKEND",
                            # VK-P13-SELECTOR-1 / T4: fail-soft proof knob.
                            "MC2_VK_FORCE_FAIL",
                            # VK-P13-SELECTOR-1 / T3+T5: enable the VK present
                            # shared-image readback content check (proves the
                            # presented frame is the real, changing GL frame).
                            "MC2_VK_PRESENT_VERIFY",
                            # VK-P14b: copy-1 elimination. Redirect endScene's
                            # backbuffer bind to the shared present FBO so the
                            # composite renders straight into the shared image
                            # (skips copy-1, Y-flip moves into copy-2). default
                            # OFF; must be allowlisted or Popen drops it and the
                            # gate-ON present-parity run silently runs copy-1.
                            "MC2_VK_PRESENT_DIRECT_FBO",
                            # VK-POST-COMPOSITE-NATIVE-1 prep: viewmode envs are
                            # needed for ObjectIdDebug/Thermal/LowLight composite
                            # parity coverage once MC2_VK_POST_COMPOSITE lands.
                            "MC2_VIEWMODE_FRAMEWORK",
                            "MC2_VIEW_MODE",
                            "MC2_OBJECT_ID_BUFFER",
                            "MC2_VIEWMODE_LOWLIGHT_GAIN",
                            "MC2_VIEWMODE_LOWLIGHT_TINT",
                            "MC2_GPU_DEBUG_NAMES",
                            # Integrated gosFX-retirement / GPU-particles plan
                            # (2026-05-20-integrated-gosfx-retirement-*):
                            # MC2_FX_TRACE = neutral fx invocation counter
                            # (mclib/fx_trace; default-OFF). Without this in
                            # the allowlist subprocess.Popen drops it and the
                            # Stage 0' content-recon trace never fires.
                            # MC2_DISABLE_GOSFX = A1 MLR work-leaf gate
                            # (default-ON since A2). Forwarded so Stage 0'
                            # captures can opt back to legacy-gosFX-ON for
                            # the histogram-baseline run only.
                            "MC2_FX_TRACE",
                            "MC2_DISABLE_GOSFX",
                            # B1 Stage 1'+: GPU particle batcher opt-in gate.
                            # (default-OFF; flipped ON for Stage 1' canaries.)
                            "MC2_GPU_PARTICLES",
                            # VFX oracle render gate (CPU-side harvest path).
                            # MC2_VFX_ORACLE_RENDER=1 enables the oracle path in
                            # CardCloud/ShardCloud/Card Draw() (default-OFF).
                            # MC2_GPU_PARTICLES_LOG=1 enables [VFX_ORACLE v1] stderr.
                            "MC2_VFX_ORACLE_RENDER",
                            "MC2_GPU_PARTICLES_LOG",
                            # VFX-DEBUG-VIEWS-1: particle billboard debug mode.
                            # 0=Final (default/byte-identical), 1=Albedo, 2=Alpha,
                            # 3=ParticleKind, 4=Overdraw, 5=Age (VFX-SHADER-AGE-FADE-PARITY-1).
                            "MC2_VFX_DEBUG_MODE",
                            # VFX-TUNING-UI-1: look-only VFX intensity scales.
                            # All default 1.0 or 0.0 (byte-identical no-ops).
                            "MC2_TUNE_VFX_BRIGHTNESS",
                            "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
                            "MC2_TUNE_VFX_ALPHA_SCALE",
                            # VFX-SHADER-AGE-FADE-PARITY-1: age-driven soft death
                            # fade for oracle particles. Default 0.0 = byte-identical.
                            "MC2_TUNE_VFX_AGE_FADE",
                            # VFX-SOFT-PARTICLES-MVP-1 / VFX-LIT-PARTICLES-MVP-1
                            "MC2_VFX_SOFT_PARTICLES",
                            "MC2_VFX_LIT_PARTICLES",
                            "MC2_TUNE_VFX_LIT_STRENGTH",
                            # StaticPropTypeTable v0 Task 5: candidate draw log
                            # proof gate. MC2_TYPE_TABLE_CAND_LOG=1 enables the
                            # [DRAW_CAND v0] summary line every frame.
                            # MC2_TYPE_TABLE_CAND_VERBOSE=1 also enables per-packet
                            # detail lines every frame (not just first-frame).
                            "MC2_TYPE_TABLE_CAND_LOG",
                            "MC2_TYPE_TABLE_CAND_VERBOSE",
                            # DrawPacket v2 per-frame field compare gate.
                            # MC2_DRAW_PACKET_COMPARE=1 enables [DRAW_PACKET_COMPARE v1]
                            # summary lines. MC2_DRAW_PACKET_COMPARE_VERBOSE=1 adds per-packet detail.
                            "MC2_DRAW_PACKET_COMPARE",
                            "MC2_DRAW_PACKET_COMPARE_VERBOSE",
                            # DrawPacket v3 ABI-promotion probe gate.
                            # MC2_DRAW_PACKET_V3=1 enables [DRAW_PACKET v3] build-stats line per frame.
                            "MC2_DRAW_PACKET_V3",
                            # DrawPacket v4B count-level coalesce coverage compare.
                            "MC2_DRAW_PACKET_COALESCE_COMPARE",
                            "MC2_DRAW_PACKET_COALESCE_VERBOSE",
                            # DrawPacket v4C slot-level coalesce coverage soak.
                            "MC2_DRAW_PACKET_COALESCE_V4C",
                            # DrawPacket v5 substitutive per-draw-call dispatch.
                            "MC2_DRAW_PACKET_COALESCE_V5",
                            "MC2_DRAW_PACKET_COALESCE_V5_TRACE",
                            # DrawPacket v6 canonical packet+meta array dispatch.
                            # (v6 opt-in gate retired in v7.1 — GATE-RETIRE-2; TRACE stays live.)
                            "MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE",
                            # DrawPacket v7 kill-switch (reverts to legacy multidraw).
                            "MC2_STATIC_PROP_LEGACY_DISPATCH",
                            # DrawPacket v8 kill-switch: =1 restores live build + snapshot compare.
                            "MC2_STATIC_PROP_LIVE_BUILDER",
                            # MaterialGpu v7 kill-switch (set =0 to disable shader sampling).
                            "MC2_MATERIAL_GPU_SAMPLE",
                            # MaterialGpu master kill-switch (set =0 to disable table upload).
                            "MC2_MATERIAL_GPU",
                            # Extraction v2.3: snapshot-assisted static-prop
                            # snap-cull gate. MC2_SNAP_CULL=1 enables the v6
                            # dispatch loop to skip draw slots whose instanceCount
                            # was zero in the previous frame's RenderSnapshot. Opt-in
                            # for testing; requires snap->ok==1. Forwarded in the
                            # allowlist so tier1 smoke runs can use this kill-switch.
                            "MC2_SNAP_CULL",
                            # Extraction v3 snapshot build gate + log gate.
                            # MC2_SNAPSHOT_STATIC_PROP_BUILD=1 enables the v3
                            # draw-packet builder from snapshot rows. Forwarded so
                            # tier1 forced-ON canaries work correctly.
                            # MC2_RENDER_SNAPSHOT_LOG=1 enables the per-frame
                            # [RENDER_SNAPSHOT v3] + [v3 build] log lines (used
                            # to verify spBuild* counters in smoke step 2).
                            "MC2_SNAPSHOT_STATIC_PROP_BUILD",
                            "MC2_RENDER_SNAPSHOT_LOG",
                            # MC2_SNAPSHOT_MECH_EXTRACT=1 enables MECH-EXTRACTION-0
                            # persist buffer + compare counters (gate OFF by default).
                            "MC2_SNAPSHOT_MECH_EXTRACT",
                            # F1-3A ViewUniforms UBO upload gate (default-OFF).
                            # MC2_VIEW_UNIFORMS=1 uploads per-frame view matrices
                            # to UBO at binding=3. No shader consumption yet (F1-3B).
                            "MC2_VIEW_UNIFORMS",
                            # StaticPropAmbientV1 hemisphere ambient term (default-OFF).
                            # MC2_STATIC_PROP_AMBIENT_V1=1 enables the ambient V1 path.
                            "MC2_STATIC_PROP_AMBIENT_V1",
                            # FAR-BACKDROP-LIT night-shade substitution for far/impostor
                            # static-prop LODs (default-OFF). Gate + distance-band +
                            # strength + debug tint tunables. OFF -> byte-identical.
                            "MC2_FAR_BACKDROP_LIT",
                            "MC2_FAR_BACKDROP_LIT_NEAR",
                            "MC2_FAR_BACKDROP_LIT_FAR",
                            "MC2_FAR_BACKDROP_LIT_STRENGTH",
                            "MC2_FAR_BACKDROP_LIT_TINT",
                            # EDGE-LIGHT-EMISSION-1: restore the stock hot-colour
                            # NIGHT vertex tags on the GPU static-prop path (runway
                            # edge lights / beacons / lit windows). Default-OFF ->
                            # isNight/factor uploaded as 0 -> byte-identical.
                            # FIXTURE-GLOW-1: bounded distance-attenuated
                            # additive glow on the same fixture types.
                            "MC2_STATIC_PROP_FIXTURE_GLOW",
                            "MC2_STATIC_PROP_FIXTURE_LIGHTS",
                            "MC2_STATIC_PROP_NIGHT_TAGS",
                            "MC2_STATIC_PROP_NIGHT_TAGS_SCALE",
                            # BACKDROP-BLIT-ATTRIB-1: night-dim the edge-fog +
                            # OOB horizon cloud bank (default-OFF, byte-identical).
                            "MC2_NIGHT_FOG_DIM",
                            "MC2_NIGHT_FOG_DIM_SCALE",
                            # V-MATERIAL-DEBUG-1 gated debug material views
                            # (default-OFF). 1=albedo, 2=materialIdx, 3=normal,
                            # 4=texArrayLayer, 5=roughness, 6=metallic.
                            # Clamped 0..6 by CPU.
                            "MC2_STATIC_PROP_DEBUG_MATERIAL",
                            # V-IBL-STATIC-1 SH-L2 image-based ambient (default-ON).
                            # MC2_STATIC_PROP_IBL_SH=0 disables; ImGui slider
                            # tunes u_iblShStrength when the gate is on.
                            "MC2_STATIC_PROP_IBL_SH",
                            # V-IBL-STATIC-1-SOAK: optional default strength
                            # override (clamped 0..3). Only meaningful when
                            # MC2_STATIC_PROP_IBL_SH=1. ImGui slider may
                            # still override at runtime.
                            "MC2_STATIC_PROP_IBL_SH_STRENGTH",
                            # IBL-NIGHT-SCALE-1: night floor for the day-sky SH
                            # probe (default 0.25, DEFAULT-ON). "1.0" restores
                            # the pre-slice byte-identical behaviour.
                            "MC2_STATIC_PROP_IBL_NIGHT_SCALE",
                            # V-IBL-STATIC-2: optional SH-set override by name
                            # (e.g. "default"). Unset/empty/unknown falls back
                            # to mission registry or default set. Dev knob.
                            "MC2_STATIC_PROP_IBL_SH_SET",
                            # IBL-ROTATION-LAND-1: rotate the SH-IBL probe onto
                            # the mission sun azimuth, CPU-side at the upload
                            # site (default-OFF). Without this passthrough row a
                            # gate-ON A/B would silently grade OFF against OFF.
                            "MC2_STATIC_PROP_IBL_SUN_ROTATE",
                            # ...and its companion floor (default 0.45, in the
                            # probe's irradiance units). Inert unless the gate
                            # above is on; "0" gives the bare rotation.
                            "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR",
                            # V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel
                            # + power-lobe specular gate (default-OFF).
                            # MC2_STATIC_PROP_PBR_V1=1 enables; ImGui slider
                            # tunes u_pbrV1Strength when on.
                            "MC2_STATIC_PROP_PBR_V1",
                            # V-MATERIAL-PBR-3: optional default-strength
                            # override (clamped 0..3). Only meaningful when
                            # MC2_STATIC_PROP_PBR_V1=1.
                            "MC2_STATIC_PROP_PBR_V1_STRENGTH",
                            # V-MATERIAL-PBR-3-DIAG: diagnostic sunFound
                            # visualizer (cyan/magenta). Default-OFF; only
                            # meaningful with MC2_STATIC_PROP_PBR_V1=1.
                            "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
                            # STATICPROP-MATERIAL-ORM-1: per-bucket linear ORM
                            # (occlusion-roughness-metallic) texture slots +
                            # sidecar feed (default-OFF). =1 enables.
                            "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
                            # STATICPROP-MATERIAL-ORM-1: ORM strength tuning
                            # (only meaningful with MC2_STATICPROP_MATERIAL_PBR_SLOTS=1).
                            "MC2_STATICPROP_ORM_STRENGTH",
                            # Render-contract Phase 2 assert mode (default-OFF).
                            # MC2_RENDER_CONTRACT_ASSERT=1 enables runtime GL state
                            # validation against declared render_contract expectations.
                            "MC2_RENDER_CONTRACT_ASSERT",
                            # [RENDER_PASS v1] advisory per-pass telemetry
                            # (slice D1). Default-OFF; =1 emits one
                            # [RENDER_PASS v1] line per pass every 300 frames.
                            # Log-only; never affects verdicts. (Allowlist
                            # entry is convention/documentation — runner.py
                            # inherits the parent env via os.environ.copy().)
                            "MC2_RENDER_PASS_TELEMETRY",
                            # DEBUG-STATE-DUMP-1: JSON render-state snapshots.
                            # Default-OFF; =1 writes debug_state/latest_render_state.json.
                            "MC2_DEBUG_STATE_DUMP",
                            # DEBUG-STATE-DUMP-1: override output directory.
                            "MC2_DEBUG_STATE_DUMP_DIR",
                            # DEBUG-STATE-DUMP-2: rolling 8-slot history ring.
                            "MC2_DEBUG_STATE_DUMP_HISTORY",
                            # STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: independent
                            # registry cache + field-by-field compare probe.
                            # Default-OFF; =1 emits [SNAPSHOT_BRIDGE_COMPARE v1]
                            # per-frame to stderr.
                            "MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
                            # STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1: clean-generation
                            # fast path that skips fillStaticPropSlots + WriteLoop
                            # and memcpy's cached rows into the snapshot arena.
                            # Default-OFF; =1 enables the dirty-only path.
                            "MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY",
                            # Terrain fast-path drop transition log (default-OFF).
                            # MC2_FASTPATH_DROP_LOG=1 emits [FASTPATH_DROP] on
                            # armed<->fallback transitions in terrain.cpp geometry().
                            "MC2_FASTPATH_DROP_LOG",
                            # MC2_MOVE_RECON=1: per-frame + atexit pathfinding
                            # cost instrumentation (move_recon.h/cpp). Default-OFF.
                            # Popen replaces env — must be in allowlist.
                            "MC2_MOVE_RECON",
                            # MC2_MOVE_CHUNK_SHADOW=1: alt-enable for the local-A*
                            # chunk/rect-corridor shadow (move_recon.cpp). Default-OFF.
                            "MC2_MOVE_CHUNK_SHADOW",
                            # MC2_MOVE_PATH_CACHE_SHADOW=1: same-query path-cache
                            # would-hit / nodes-saved / mover-divergence shadow.
                            "MC2_MOVE_PATH_CACHE_SHADOW",
                            # MC2_MISSION_SPLIT=1: wall-ms split of Mission::update
                            # sub-calls (land_update/pathmgr/clearVerts/geometry/
                            # objmgr...) to locate the 1K-map logic hotspot.
                            "MC2_MISSION_SPLIT",
                            "MC2_WATER_S6_COST",
                            "MC2_QUADSETUP_ARMED_SKIP",
                            "MC2_GEOM_PHASE_SPLIT",
                            "MC2_MIF_SPLIT",
                            # MISSION-INTERFACE-PERF-1: [IFACE_PERF v1] windowed
                            # cost split of MissionInterfaceManager::update()
                            # (default OFF). Must be allowlisted or Popen drops
                            # it and the gate-ON smoke emits nothing.
                            "MC2_IFACE_COST_SPLIT",
                            # [PICK_CAP] per-walk numTiles/admitted/contained
                            # trace in Camera::inverseProject (pre-existing,
                            # default OFF) -- sizes the picker walk stages.
                            "MC2_PICK_CAP_TRACE",
                            # MISSION-INTERFACE-PERF-1: coarse-to-fine
                            # closest-vertex fallback picker (default OFF).
                            # Must be allowlisted or Popen drops it and the
                            # gate-ON smoke measures the legacy brute force.
                            "MC2_PICK_FALLBACK_COARSE",
                            "MC2_PICK_RECON",
                            # Diagnostic JSONL trace (diagnostic_trace.cpp).
                            # MC2_DIAGNOSTIC_TRACE_FILE is handled above (absolute
                            # path override) so only the tag filter needs passthrough.
                            "MC2_DIAG_TAGS",
                            # PATHFINDING-JUMP-FAIL-1: per-pilot doomed jump-path
                            # backoff gate (default-OFF killswitch). Without this in
                            # the allowlist Popen drops it and the gate-ON smoke runs
                            # an engine that never backs off (numbers match OFF).
                            "MC2_PATH_JUMP_FAIL_BACKOFF",
                            # PATHFINDING-SOLVER-ISOLATION-1: per-solve SolveContext
                            # gate (default-OFF). Without this in the allowlist Popen
                            # drops it and the gate-ON path-trace proof runs the OFF
                            # engine (traces match trivially, proving nothing).
                            "MC2_PATH_SOLVE_ISOLATED",
                            # VEG-SMOKE-FLOOR-1: vegetation card output contract gate.
                            # MC2_VEGETATION_CARDS=1 enables GPU instanced vegetation
                            # and triggers VEG-SMOKE-FLOOR-1 assertions in this runner.
                            # Without this entry Popen drops it and the floor check
                            # compares against an engine that never drew vegetation.
                            "MC2_VEGETATION_CARDS",
                            # VEG-FLUSH-REASON-1: atlas path override for vegetation tests.
                            "MC2_VEGETATION_ATLAS",
                            # FRAME-JOBS-2x: parallel touch/bounds pre-pass gates.
                            # Without these in the allowlist Popen drops them and
                            # the gates never reach the engine.
                            "MC2_FRAME_JOBS",
                            "MC2_FRAME_JOBS_WORKERS",
                            "MC2_FRAME_JOBS_BATCH",
                            "MC2_FRAME_JOBS_TRACE",
                            "MC2_FRAME_JOBS_TOUCH",
                            "MC2_FRAME_JOBS_TOUCH_DIAG",
                            # FRAME-JOBS-2F: touch-entry unification stamp diagnostics.
                            "MC2_FRAME_JOBS_TOUCH_DIAG",
                            # FRAME-JOBS stable-light skip gate.
                            "MC2_LIGHTBRIDGE_STABLE_SKIP",
                            # STABLE-LIGHT-SKIP-BROADEN-1 (framebudget lane): broaden
                            # the LBSS resubmit-skip to the legacy touch() Path-B
                            # variants (live path in stock config). Requires
                            # MC2_STABLE_LIGHT_SKIP armed for eligibility + the
                            # TOUCH gate to take the skip. Without these allowlisted
                            # the gate-ON A/B is inert.
                            "MC2_STABLE_LIGHT_SKIP",
                            "MC2_STABLE_LIGHT_SKIP_DIAG",
                            "MC2_STABLE_LIGHT_SKIP_TOUCH",
                            # STATIC-SCENE-PROXY-RECON-1: proxy candidate classifier.
                            "MC2_STATIC_PROXY_RECON",
                            # STATIC-REGISTRY-COVERAGE-RECON-1: sub-classify rej_no_static_reg.
                            "MC2_STATIC_REG_COVERAGE",
                            # STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
                            # MC2_STATIC_REG_PREWARM=1 enables prewarmStaticPropLightBakes().
                            # MC2_STATIC_REG_PREWARM_TRACE=1 adds per-object diagnostic.
                            "MC2_STATIC_REG_PREWARM",
                            "MC2_STATIC_REG_PREWARM_TRACE",
                            # BRAIN-TASKQ-1 / BRAIN-RUNTIME-1A: ABL brain task queue
                            # and runtime mode gates. Without these in the allowlist
                            # Popen drops them and gate-ON smokes run with the feature
                            # fully inert (gate-OFF behavior), masking integration regressions.
                            "MC2_BRAIN_TASKQ",
                            "MC2_BRAIN_TASKQ_TRACE",
                            "MC2_BRAIN_RUNTIME",
                            "MC2_BRAIN_RUNTIME_TRACE",
                            "MC2_BRAIN_RUNTIME_FORCE_MODE",
                            "MC2_BRAIN_RUNTIME_APPLY",
                            "MC2_BRAIN_DISPATCH",
                            "MC2_BRAIN_DISPATCH_APPLY",
                            "MC2_BRAIN_DISPATCH_FSM_TODO",
                            "MC2_BRAIN_DISPATCH_VAR",
                            # DISPATCH-EFFECT-UNITEJECT-1: fixture override gate.
                            # MC2_BRAIN_SPECIAL_FIT=<name> redirects parseBrainSpecialBody
                            # to load <name>_specials.fit instead of <mission>_specials.fit.
                            # Required for Gates B/C/D/E eject fixture validation.
                            "MC2_BRAIN_SPECIAL_FIT",
                            "MC2_BRAIN_DISPATCH_CALL",
                            # EXECUTOR-ISLAND-SCREENSHADOW-1: frame-graph executor gate.
                            # Without this in the allowlist Popen drops it and
                            # MC2_FRAMEGRAPH_EXECUTOR=1 smoke runs are inert.
                            "MC2_FRAMEGRAPH_EXECUTOR",
                            # UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1: observe-only UI pass
                            # draw-count + ambient entry sample. Without this in the
                            # allowlist Popen drops it and the ui_pass dump block stays
                            # zero/Inherit on gate-ON smoke runs.
                            "MC2_UI_PASS_MEASURE",
                            # SMOKE-GATE-GUARD-1: frame-graph dryrun gate.
                            # Without this in the allowlist Popen drops it and
                            # MC2_FRAMEGRAPH_DRYRUN=1 smoke runs are inert.
                            "MC2_FRAMEGRAPH_DRYRUN",
                            # MEASURED-REORDER-SPMECH-1: tier-C reorder experiment
                            # gate (default-OFF). Without this in the allowlist
                            # Popen drops it and MC2_FRAMEGRAPH_REORDER_SPMECH=1
                            # smoke runs silently run the default (OFF) order.
                            "MC2_FRAMEGRAPH_REORDER_SPMECH",
                            # DETERMINISTIC-RNG-1: seedable LCG gate + seed override.
                            # Without these in the allowlist Popen drops them and the
                            # gate-ON determinism smoke runs the CRT (gate-OFF) path,
                            # making the RNG trace non-reproducible.
                            "MC2_DETERMINISTIC_RNG",
                            "MC2_RNG_SEED",
                            "MC2_VULKAN_SPV_DIR",
                            # VK-WATER-TIER1 (rung 6): in-place shared-GBuffer water
                            # co-write (color0+normal+depth LOAD-preserve; premult-over
                            # blend == GL straight-alpha result; no composite, no depth
                            # readback). envTriState OPT-IN, default-OFF (same new-rung
                            # pattern as MC2_VK_TERRAIN_SKIRTS / _VISUALDISPLACE_FAR).
                            "MC2_VK_WATER_TIER1",
                            # VK-FIELD-VFX-1: default-ON-under-umbrella particle
                            # island gate (weapon fire into shared color0). Must be
                            # allowlisted so an explicit MC2_VK_VFX=0 override reaches
                            # the child (else Popen drops it).
                            "MC2_VK_VFX",
                            # VK-VFX-COALESCE-1: HUD tri-batch merge killswitch
                            # (default OFF pending parity verification). Must be
                            # allowlisted so an explicit override reaches the child.
                            "MC2_VK_VFX_COALESCE",
                            # MC2_VK_WATER_PERF: fine-grained per-sub-step us breakdown
                            # of the VK water interop path ([VK_WATER_PERF] lines).
                            # Additive diagnostic; must be allowlisted or Popen drops it
                            # and the breakdown never emits.
                            "MC2_VK_WATER_PERF",
                            # VK-WATER-FENCE-DEFER-1 killswitch (=0 restores the inline
                            # post-submit fence wait; default = deferred retire at the
                            # parity slot's next beginFrame).
                            "MC2_VK_WATER_FENCE_DEFER",
                            # VK-TERRAIN-FENCE-DEFER-1: same template on the terrain
                            # island (killswitch =0 restores the inline wait) + the
                            # water-style [VK_TERRAIN_PERF] us-breakdown diagnostic.
                            "MC2_VK_TERRAIN_FENCE_DEFER",
                            "MC2_VK_TERRAIN_PERF",
                            # VK-PROP-FENCE-DEFER-1: same template on the prop island
                            # (killswitch =0 restores the inline wait) + the
                            # [VK_PROPS_PERF] us-breakdown diagnostic.
                            "MC2_VK_PROP_FENCE_DEFER",
                            "MC2_VK_PROP_PERF",
                            # VK-MECH-FENCE-DEFER-1: same template on the mech island.
                            "MC2_VK_MECH_FENCE_DEFER",
                            "MC2_VK_MECH_PERF",
                            # VK-LIGHTFEED-DEDUP-1: killswitch (=0 restores the twin
                            # per-island LightsData copies; default = ONE shared
                            # per-frame SSBO ring consumed by mech + prop islands).
                            "MC2_VK_LIGHTFEED_DEDUP",
                            "MC2_NO_LAUNCHER",
                            # VK-P8a SHADOW-INTEROP: VK produces the dynamic shadow
                            # depth map, GL samples it. Requires the shared engine
                            # VulkanContext (MC2_RENDER_BACKEND=vulkan). Without this
                            # in the allowlist Popen drops it and the gate-ON smoke
                            # silently runs the GL shadow path (gate inert).
                            "MC2_VK_SHADOW_INTEROP",
                            # VK-P14 GATE-CONSOLIDATION-1: THE UMBRELLA. =1 arms the
                            # whole shared-GBuffer scene family at once (all sub-gates
                            # below default ON; explicit sub-gate env still overrides).
                            # The eventual default flip toggles EXACTLY this one name, so
                            # the soak must pass it through to exercise the final gate.
                            "MC2_VK_SHARED_GBUFFER",
                            # VK shared-scene deletion-readiness canary. When =1,
                            # any requested shared scene color/depth/normal/objectId
                            # slot that falls back to plain GL aborts, preventing
                            # false-positive final-tail/Metal-gate proofs.
                            "MC2_VK_SHARED_SCENE_STRICT",
                            # VK-TIER1 SHARED SCENE DEPTH: backs sceneDepthTex_ with a
                            # VK-built exportable D32S8 image imported into GL as
                            # GL_DEPTH24_STENCIL8 at the scene-FBO depth alloc+attach
                            # site (GL-only slice; no VK depth consumer yet). Requires
                            # the shared engine VulkanContext. Without this in the
                            # allowlist Popen drops it and the gate-ON smoke silently
                            # runs the plain GL depth alloc (gate inert / fail-soft).
                            "MC2_VK_SHARED_SCENE_DEPTH",
                            # VK-STATIC-SHADOW-MAP: backs ShadowStaticMap with a
                            # VK-built exportable D32 image imported into GL. GL still
                            # renders/samples it; native screen-shadow later can bind
                            # the same VkImageResource. Must be allowlisted or the
                            # substrate proof silently runs the plain GL allocation.
                            "MC2_VK_STATIC_SHADOW_MAP",
                            # VK-P11 TIER-1 PROP COLOR: co-writes props IN-PLACE into
                            # the shared scene color(MRT0)+normal(MRT1)+depth GBuffer
                            # (no side-image/composite). Requires the shared engine
                            # VulkanContext AND MC2_VK_SHARED_SCENE_DEPTH. Without this
                            # in the allowlist Popen drops it and the gate-ON smoke
                            # silently runs the GL flush() prop path (gate inert).
                            "MC2_VK_PROP_COLOR_TIER1",
                            # VK-PROP-DIRECT-CHAIN: deletion-readiness proof gate
                            # that keeps shared GBuffer ownership on Vulkan after the
                            # native terrain prefix and skips prop interop semaphores.
                            "MC2_VK_PROP_DIRECT_CHAIN",
                            # VK-PROP-MATERIAL: real textured vertex-baked-lit prop
                            # materials (both albedo arrays). Requires MC2_VK_PROP_COLOR_TIER1.
                            "MC2_VK_PROP_MATERIAL",
                            "MC2_VK_PROP_MATERIAL_DEBUG",
                            # VK-P14 MECH TIER-1: the first VK-native SKINNED region —
                            # co-writes the GL mech batcher's visible instances IN-PLACE
                            # into the shared GBuffer, VK-skinned (LBS + MC2 axis swap).
                            # Requires MC2_VK_SHARED_SCENE_DEPTH. Sub-slice 1 = skinning +
                            # flat color; MC2_VK_MECH_MATERIAL adds the copy-once slot-0
                            # paint-scheme albedo (single-material probe).
                            "MC2_VK_MECH_TIER1",
                            # VK-MECH-DIRECT-CHAIN: deletion-readiness proof gate
                            # that keeps shared GBuffer ownership on Vulkan after
                            # terrain+prop and skips mech interop semaphores.
                            # VK-WATER-DIRECT-CHAIN: deletion-readiness proof gate
                            # that keeps shared GBuffer ownership on Vulkan after
                            # terrain+prop+mech and skips water interop semaphores.
                            "MC2_VK_WATER_DIRECT_CHAIN",
                            # VK-POST-COMPOSITE-DIRECT-CHAIN: deletion-readiness
                            # proof gate that consumes VK-owned shared scene color0
                            # without a composite-stage GL interop handoff.
                            "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN",
                            "MC2_VK_MECH_MATERIAL",
                            # MC2_VK_MECH_LIGHTING: per-vertex calc_light Lambert +
                            # per-actor fog, ported from the GL default mech path
                            # (albedo * v_litColor + highlight, fogged). Requires
                            # MC2_VK_MECH_MATERIAL for the visible albedo multiply.
                            "MC2_VK_MECH_LIGHTING",
                            # VK-MECH-FEED-NATIVE-1 (rung 7): mech island consumes the
                            # game's CPU byte sources (exact uploaded VBO blob, staging
                            # IBO, mcTextureManager lightData_) instead of reading the
                            # GL buffer objects back (glGetBufferSubData). envTriState
                            # OPT-IN, default-OFF (same new-rung pattern as
                            # MC2_VK_TERRAIN_SKIRTS / MC2_VK_WATER_TIER1).
                            "MC2_VK_MECH_FEED_NATIVE",
                            # VK-MECH-FEED-AUDIT-1: verification-only (safety-gate class
                            # like MC2_VK_TERRAIN_HEIGHT_AUDIT) -- byte-compares the CPU
                            # sources vs the GL buffers on every native copy.
                            "MC2_VK_MECH_FEED_AUDIT",
                            # VK terrain in-place shared-GBuffer region + lighting + detail.
                            "MC2_VK_TERRAIN_TIER1",
                            # VK-TERRAIN-ALL-LODS (rung 3): coarse-band (lodStep>1) capture
                            # extension, requires MC2_VK_TERRAIN_TIER1. Bands only, no
                            # skirts/stitch this rung. Default-OFF.
                            "MC2_VK_TERRAIN_ALL_LODS",
                            # VK-TERRAIN-SKIRTS (rung 4): skirt/edge-stitch geometry
                            # port, requires MC2_VK_TERRAIN_ALL_LODS. Independent
                            # kill-switch from ALL_LODS so a bad skirt port can be
                            # isolated without losing the rung-3 band win. Default-OFF.
                            "MC2_VK_TERRAIN_SKIRTS",
                            # VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): far-band mode-2
                            # visual-displace Z-swap port, requires MC2_VK_TERRAIN_ALL_LODS.
                            # Independent kill-switch (same pattern as SKIRTS). Default-OFF.
                            "MC2_VK_TERRAIN_VISUALDISPLACE_FAR",
                            # GL-side far-band fade knob (MC2_TERRAIN_VISUAL_DISPLACE
                            # itself is already allowlisted in the TERRAIN-VISUAL-HEIGHT
                            # block below; the _FAR fade knob was not).
                            "MC2_TERRAIN_VISUAL_DISPLACE_FAR",
                            "MC2_VK_TERRAIN_LIGHTING",
                            "MC2_VK_TERRAIN_DETAIL",
                            "MC2_VK_TERRAIN_LODFADE",
                            "MC2_VK_TERRAIN_DYNSHADOW",
                            "MC2_SHADOW_CSM",
                            "MC2_SHADOW_DEBUG_MODE",
                            "MC2_VK_TERRAIN_CSM",
                            "MC2_VK_CSM_FULLMAP_INTEROP",
                            "MC2_VK_CSM_ARRAY_INTEROP",
                            "MC2_VK_TERRAIN_HEIGHT_AUDIT",
                            "MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT",
                            # VK-DRAW-PERF-1: verification-only GPU timestamp segment
                            # timers on the terrain + mech island render passes (per-pass
                            # setup/clear vs draw GPU cost via vkCmdWriteTimestamp).
                            # Default-OFF; no-op + fail-soft when unset/unsupported.
                            "MC2_VK_GPU_TIMESTAMPS",
                            # VK-POST-FOG-NATIVE-1 (rung 8.1): edge+oob fog pair
                            # rendered natively on the shared engine context (samples
                            # shared scene depth, blends into shared scene color0 in
                            # place; zero copies/readbacks). envTriState OPT-IN,
                            # default-OFF (new-rung pattern as MC2_VK_TERRAIN_SKIRTS).
                            "MC2_VK_POST_FOG_NATIVE",
                            # VK-POST-SSAO-NATIVE-1 (rung 8.2): SSAO gen+apply
                            # rendered natively on the shared engine context (samples
                            # shared scene depth+normal into a VK-owned half-res R16F
                            # occlusion image, multiplies into shared scene color0 in
                            # place; zero copies/readbacks). envTriState OPT-IN,
                            # default-OFF (new-rung pattern as MC2_VK_POST_FOG_NATIVE).
                            "MC2_VK_POST_SSAO",
                            # VK-POST-SCREENFX-NATIVE-1 (rung 8.3): cloud-shadow +
                            # shoreline-foam rendered natively on the shared engine
                            # context (samples shared scene depth+normal, multiplies
                            # into shared scene color0 in place; zero copies/readbacks;
                            # Static screen-shadow/cloud/shoreline post-screenfx native path.
                            # envTriState OPT-IN, default-OFF + per-pass bisect subs.
                            "MC2_VK_POST_SCREENFX",
                            "MC2_VK_POST_SCREENFX_SHADOW",
                            "MC2_VK_POST_SCREENFX_CLOUD",
                            "MC2_VK_POST_SCREENFX_SHORELINE",
                            "MC2_CLOUD_SHADOW",
                            # VK-SHADOW-COPY-ONCE-1: diagnostic-only per-frame ms log for
                            # the VK-terrain-interop-copies zone (stderr line, default OFF).
                            "MC2_VK_TERRAIN_COPY_STATS",
                            # VK-P9a WATER-INTEROP proof: the bookmark-capture sweep
                            # env, so a smoke run (which reliably reaches steady render,
                            # unlike the standalone capture harness on some setups) can
                            # ALSO drive the camera to the water-visible bookmark poses
                            # and dump per-pose PNGs for VK-vs-GL water parity. Without
                            # these in the allowlist Popen drops them and the sweep never
                            # arms (water never framed -> island never engages).
                            "MC2_VISUAL_BOOKMARK_CAPTURE",
                            "MC2_VISUAL_CAPTURE_DIR",
                            "MC2_VISUAL_CAPTURE_FRAME",
                            "MC2_VISUAL_SETTLE",
                            # RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY): master
                            # gate + backend sub-selector for the SELECTABLE
                            # PostprocessFog region. Without these in the allowlist
                            # Popen drops them and the gate-ON smoke silently runs
                            # the original direct-GL fog sites (region iface inert).
                            # _FORCE_FALLBACK forces the VK impl to decline so the
                            # FallbackGL path is provable under smoke.
                            "MC2_RENDER_BACKEND_REGION_IFACE",
                            "MC2_POSTPROCESS_BACKEND",
                            # SSAO-GTAO-LITE-MVP-1 / VK-REGION-SSAO-1: the SSAO master
                            # gate + its debug (raw-AO) override. The PostprocessSSAO
                            # backend-compare (run_golden_parity --region PostprocessSSAO)
                            # forces MC2_SSAO=1 for BOTH states so the region actually
                            # shades in-frame; without this in the allowlist Popen drops
                            # it from golden_scene's structural capture and the compare's
                            # structural half runs with SSAO silently OFF (the region
                            # never fires, so a "pass" proves nothing). MC2_SSAO_DEBUG
                            # lets the raw occlusion buffer be inspected under smoke.
                            "MC2_SSAO",
                            "MC2_SSAO_DEBUG",
                            # SSAO-BLUR-1: the depth-aware bilateral blur killswitch
                            # (default ON with SSAO). Allowlisted so an A/B smoke can
                            # actually reach the no-denoise chain -- without it Popen
                            # drops MC2_SSAO_BLUR=0 and the "blur OFF" arm silently
                            # runs blur ON, i.e. both arms identical and the compare
                            # proves nothing (the MC2_SSAO trap fixed by 5cd3cc65).
                            "MC2_SSAO_BLUR",
                            "MC2_SSAO_BLUR_SIGMA",
                            # SSAO-BLUR-1 housekeeping: MC2_VISUAL_TUNING_FILE points
                            # the visual-tuning profile reader (visual_tuning_profile.cpp)
                            # at an alternate JSON, which is how aoRadius/aoStrength/
                            # aoBias (and every other profile key) are graded WITHOUT a
                            # rebuild. It had a live reader but was never registered,
                            # documented, or allowlisted -- so any smoke-driven tuning
                            # sweep silently graded the built-in defaults instead of the
                            # supplied file. Not a renderer gate; a tuning-input path.
                            "MC2_VISUAL_TUNING_FILE",
                            # HZB-DEPTH-PYRAMID-MVP-1 / VK-REGION-HZB-1: the HZB build
                            # gate (+ its CPU cross-check probe). The PostprocessHzb
                            # backend-compare (run_golden_parity --region PostprocessHzb)
                            # forces MC2_HZB_BUILD=1 for BOTH states so the region
                            # actually builds in-frame; without this in the allowlist
                            # Popen drops it and the compare runs with HZB silently OFF
                            # (region_impl=None -- the compare proves nothing; same trap
                            # class as the MC2_SSAO gap fixed by 5cd3cc65).
                            # VK-POST-HZB-NATIVE-1: the rung-8.4 native HZB gate.
                            # Needs the build gate too; probe mode intentionally falls
                            # back to GL so the GL pyramid stays coherent for reads.
                            "MC2_VK_POST_HZB",
                            # VK-POST-COMPOSITE-NATIVE-1 (rung 8.5b): native
                            # composite/tonemap/FXAA/viewmode gate. Runtime hook
                            # lands after shader asset prep; allowlisted now so
                            # gate-on smoke cannot silently run the GL composite.
                            "MC2_VK_POST_COMPOSITE",
                            "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN",
                            "MC2_VK_OBJECTID_DIRECT_CHAIN",
                            "MC2_FXAA",
                            "MC2_FXAA_SUBPIX",
                            "MC2_FXAA_EDGE_THRESHOLD",
                            "MC2_FXAA_EDGE_THRESHOLD_MIN",
                            "MC2_FORCE_43",
                            # VK-TAIL-HOOK-1: behavior-neutral final-tail command
                            # buffer seam; proves native HUD/ImGui can execute
                            # after GL->VK handoff and before the swapchain blit.
                            "MC2_VK_PRESENT_TAIL_HOOK",
                            # VK present no-import proof gate: allocates the
                            # present target as VK-only when the native strict
                            # stack owns composite/HUD/ImGui.
                            "MC2_VK_PRESENT_NO_GL_IMPORT",
                            # VK-PRESENT-SKIP-GL-HANDOFF-1: explicit proof that
                            # copy-2 can consume a VK-owned present image without
                            # a GL->VK semaphore signal when native composite ran.
                            "MC2_VK_PRESENT_SKIP_GL_HANDOFF",
                            # VK present-tail deletion-readiness canary. On
                            # nonzero-surface frames, aborts if copy-1 or the
                            # GL->VK handoff would run instead of the VK-owned
                            # native present path. Minimized smoke remains a
                            # non-proof and does not trip this canary.
                            "MC2_VK_PRESENT_NATIVE_STRICT",
                            # VK final Metal-gate canary: hard-fails on any
                            # glInteropSignalToVk/glInteropWaitFromVk boundary.
                            # Expected to fail until full VK ownership deletes
                            # the Win32 GL/VK interop substrate.
                            "MC2_VK_NO_GL_INTEROP_STRICT",
                            # Stronger Metal-gate resource canary: hard-fails
                            # exportable Win32 VkImageResource creation / GL
                            # memory-object import, not just handoff calls.
                            "MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT",
                            # VK-HUD-NATIVE-QUEUE-1 (rung 8.6f): queues the
                            # ordered GameOS HUD batch to the Vulkan present tail.
                            "MC2_VK_HUD",
                            # VK-HUD-TEXTURE-PROOF-1: native present-tail HUD
                            # proof renderer for untextured + mirrored textured
                            # records. Without this, smoke only proves queueing.
                            "MC2_VK_HUD_COLOR_PROOF",
                            # VK-HUD-SKIP-GL-1: skip legacy GL HUD replay only
                            # after the queued batch is fully native-representable.
                            "MC2_VK_HUD_SKIP_GL",
                            # VK-IMGUI-NATIVE-1 (rung 8.6): ImGui Vulkan renderer
                            # gate. Current slice is observable-decline only until
                            # the final-tail present handoff is split; must be
                            # allowlisted so smoke proves the gate reaches runtime.
                            "MC2_VK_IMGUI",
                            "MC2_HZB_BUILD",
                            "MC2_HZB_PROBE",
                            "MC2_VK_POST_SCREENFX_SHADOW",
                            "MC2_SHADOW_ENABLE",
                            "MC2_SHADOW_CSM",
                            "MC2_VK_SHADOW_INTEROP",
                            "MC2_VK_CSM_ARRAY_INTEROP",
                            "MC2_VK_CSM_FULLMAP_INTEROP",
                            "MC2_VK_STATIC_SHADOW_MAP",
                            # SKYBOX-FOG-EXCLUDE-1: stencil-tag true sky pixels during
                            # the HDRI skybox draw so runEdgeFog/runFogOob hard-exclude
                            # them. DEFAULT ON since SKYBOX-FOG-EXCLUDE-DEFAULT-ON-1; the
                            # byte-identical arm is the explicit "=0" kill, not unset.
                            # Without this in the allowlist Popen drops it and a smoke
                            # run driving "=0" silently keeps the exclusion ON.
                            "MC2_SKYBOX_FOG_EXCLUDE",
                            # VULKAN-ISLAND-VALIDATION-WIRING-1: opt-in Vulkan
                            # validation preset (off/core/sync/gpu-assisted/
                            # best-practices/debug-printf). Without this in the
                            # allowlist Popen drops it and the island's validation
                            # layer never loads, so validation_errors stays 0 by
                            # omission rather than by being genuinely clean.
                            "MC2_VULKAN_VALIDATION",
                            # VULKAN-SWAPCHAIN-PRESENT-1: Layer-5 gate. MC2_VULKAN_PROBE
                            # runs the one-shot Vulkan probe suite at startup;
                            # MC2_VULKAN_SWAPCHAIN_PRESENT additionally runs the
                            # swapchain PRESENT path (own window, 16 frames, resize);
                            # _HIDDEN makes that window headless for CI. Without these
                            # in the allowlist Popen drops them and the present probe
                            # never runs (no [VK_SWAPCHAIN_PRESENT_HEALTH] line).
                            "MC2_VULKAN_PROBE",
                            "MC2_VULKAN_SWAPCHAIN_PRESENT",
                            "MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN",
                            # TERRAIN-VISUAL-HEIGHT-CONSUMER-1: visual-height
                            # displacement gate (default-OFF; loader also
                            # accepts MC2_TERRAIN_VISUAL_HEIGHT for load-only).
                            # Without these in the allowlist Popen drops them
                            # and the gate-ON smoke silently runs the coarse
                            # (undisplaced) LOD0 path -- no [VISUAL_HEIGHT v1]
                            # LOADED line, no SSBO upload.
                            "MC2_TERRAIN_VISUAL_DISPLACE",
                            "MC2_TERRAIN_VISUAL_HEIGHT",
                            "MC2_TERRAIN_VISUAL_HEIGHT_FILE",
                            # WATER-HDRI-REFL-PERF-1: MC2_WATER_HDRI_REFL_FULL=1
                            # restores the old full-rate LOD-1.0 HDRI reflection
                            # for A/B perf checks; MC2_WATER_HDRI_LOD overrides
                            # the sample LOD directly. Without these in the
                            # allowlist Popen drops them and the A/B smoke both
                            # run at the new default (LOD 4.0).
                            "MC2_WATER_HDRI_REFL_FULL",
                            "MC2_WATER_HDRI_LOD",
                            # WATER-REFLECTION-CLIP-1: MC2_WATER_REFLECTION_RT
                            # arms the quarter-res terrain reflection RT fill
                            # pass (default OFF); MC2_WATER_REFL_RT_PIXELPROOF
                            # opts into the old whole-RT glReadPixels coverage
                            # breakdown for deep debug (default: cheap
                            # mirrored_cmd_count-only proof). Without these in
                            # the allowlist Popen drops them and gate-ON smoke
                            # silently runs with the RT pass off / the pixel
                            # proof unreachable.
                            "MC2_WATER_REFLECTION_RT",
                            "MC2_WATER_REFL_RT_PIXELPROOF",
                            # POST-FX-TONEMAP-V2 (SCOUR-1 #3): filmic tonemap
                            # curve select (0 off / 1 agx / 2 hable / 3 reinhard,
                            # default OFF). Visual-only. Without this in the
                            # allowlist Popen drops it and a gate-ON A/B smoke
                            # silently runs the OFF (byte-identical) branch --
                            # every mode would capture identical frames.
                            "MC2_TONEMAP_V2",
                            # SLICE 3a: cliff tessellation plumbing gate + max-tess
                            # knob (default OFF). Without these in the allowlist
                            # Popen drops them and a gate-ON perf/parity smoke
                            # silently runs the GL_TRIANGLES (gate-OFF) path.
                            "MC2_TERRAIN_CLIFF_DISPLACE",
                            "MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS",
                            # VERIFY-INTEGRITY-3: registered Feature gate
                            # (RendererFeatureRegistry.h, VEHICLE-NIGHT-LIT-1,
                            # defaultOn=false) read at mclib/txmmgr.cpp:2199,
                            # documented in tier1_env_vars.md -- but absent
                            # here, so the harness could not DRIVE it and every
                            # run that set it tripped a spurious [ENV-DROP]
                            # "gate inert" warning. That is what voided the
                            # "verified via smoke" claim for this gate: the A/B
                            # was never shown to have driven the two arms apart.
                            "MC2_LEGACY_LIT_MC2_FRAME",
                            # VEHICLE-NIGHT-LIT-2: default-OFF opt-in that adds
                            # the missing else-branch clearing the GV
                            # SetLightsOut latch (mclib/gvactor.cpp:3100-3117).
                            # LIGHTING-INCAPABLE-BATCH-2: the latch is NOT
                            # shader-side -- TG_Shape::MultiTransformShape bakes
                            # it into the aRGBLight vertex attribute
                            # (tgl.cpp:1926/:2129 -> :2481/:2776) and that kernel
                            # is live (useVertexLighting=true terrain.cpp:353,
                            # Environment.Renderer=0 mechcmd2.cpp:2974). So the
                            # gate DOES move pixels, and every A/B that set it
                            # while it was missing from this list graded OFF
                            # against OFF -- which is why it was mis-filed as
                            # "arithmetically incapable" in
                            # .claude/LIGHTING-HEADROOM-AUDIT.md section 3 entry 9.
                            "MC2_GV_LIGHTSOUT_CLEAR",
                            # VEHICLE-LIGHTING-PATH-1: default-ON killswitch for
                            # the GVAppearance CacheGpuLightData() call that
                            # finally populates the vehicle light row. Must be
                            # passthrough-allowlisted or an A/B run setting =0
                            # silently runs the =1 path (ENV_DROP class).
                            "MC2_VEHICLE_LIT_PATH",
                            # VEHICLE-AMBIENT-FLOOR-1: default-ON killswitch for
                            # the mech.vert ambient floor (0.35) + clamp(0,1)
                            # mirrored onto the legacy lit vehicle path. Must be
                            # passthrough-allowlisted or an A/B run setting =0
                            # silently runs the =1 path (ENV_DROP class) and
                            # grades ON against ON.
                            "MC2_VEHICLE_AMBIENT_FLOOR",
                            # LIGHTROW-FRESHNESS-1: default-ON killswitch for the
                            # cachedFrame_ staleness check on the DYNAMIC static-prop
                            # submit path (gos_static_prop_batcher.cpp). Must be
                            # passthrough-allowlisted or an A/B run setting =0
                            # silently runs the =1 path (ENV_DROP class) and grades
                            # the fix against itself -- which for this gate would
                            # hide the one arm that matters, since =0 is the only
                            # way to reproduce the stale-row read on purpose.
                            "MC2_STATIC_PROP_LIGHT_FRESHNESS",
                            # MECH-LIGHTROW-FRESHNESS-1: default-ON killswitch
                            # for the cachedFrame_ staleness check on the GPU
                            # MECH submit path (mclib/mech3d.cpp). Same ENV_DROP
                            # reasoning as the static-prop gate above: =0 is the
                            # only way to reproduce the stale-row read on
                            # purpose, so an A/B that could not drive it would
                            # grade the fix against itself. Additionally load-
                            # bearing here because =0 vs =1 changes which PATH
                            # draws the mech (GPU batch vs CPU MLR), so a
                            # dropped env silently changes draw-call counts too.
                            "MC2_MECH_LIGHT_FRESHNESS",
                            # SUN-AXIS-FIX-1: default-OFF opt-in for the
                            # Stuff->MC2 swap on ObjectLights.light_dir in
                            # calc_light's DIFFUSE directional branches (mech +
                            # legacy-lit vehicle paths). Must be passthrough-
                            # allowlisted or the A/B that is supposed to DECIDE
                            # its default silently runs OFF against OFF and
                            # captures identical frames -- the ENV_DROP class,
                            # found twice on 2026-07-30 alone.
                            "MC2_SUN_AXIS_FIX",
                            # SUN-ELEVATION-FIX-1: default-OFF opt-in for the
                            # true-spherical Camera::lightDirection (the shipped
                            # one normalizes AFTER, so its elevation is
                            # atan(sin(pitch)), not pitch). Drives terrain, prop
                            # sun, SHADOW direction and the sun colour byte, so
                            # an A/B that could not DRIVE it would grade the
                            # whole world OFF against OFF.
                            "MC2_SUN_ELEVATION_FIX",
                            # CPU-PHONG-RETIRE-1: default-ON killswitch that
                            # retires the orphaned CPU Phong term in
                            # MapData::terrainElevation() (mclib/mapdata.cpp).
                            # This one moves GAMEPLAY elevation -- grounding,
                            # pathfinding and collision -- by up to 12.7-18.9 wu
                            # (~3.8 m) on curved ground, so the "=0" revert arm
                            # is the only way to grade a suspected regression.
                            # It was read by code but absent from this allowlist,
                            # so a smoke setting =0 silently ran the default ON
                            # path and graded ON against ON -- the ENV_DROP class
                            # (same defect as MC2_VEHICLE_AMBIENT_FLOOR above).
                            "MC2_CPU_PHONG_RETIRE",
                            # QUIET-DEFAULT-BUILD-1: MC2_DIAG_VERBOSE restores
                            # the ~380 developer diagnostic call sites that a
                            # default build now keeps quiet. Allowlisted so the
                            # harness can DRIVE it (capture a verbose log on
                            # demand) and so setting it stops tripping a
                            # spurious [ENV-DROP] "gate inert" warning.
                            # NOTE: the quiet ratchet deliberately refuses to
                            # grade any run that sets it -- see
                            # scripts/check-default-build-quiet.py.
                            "MC2_DIAG_VERBOSE",
                            # PERF-HARNESS-TRUTH-1 (2026-07-25): the MASTER gate
                            # for the night light-pool feature (registered,
                            # default ON, 3 code readers) was absent here, so a
                            # harness could not DRIVE it -- the OFF arm of every
                            # light-pool A/B silently ran the ON path. Its
                            # dependants MC2_LIGHT_POOL_MAX / MC2_LIGHT_GRID
                            # were allowlisted while the switch that makes them
                            # mean anything was not.
                            "MC2_TERRAIN_LIGHT_POOLS",
                            # PERF-HARNESS-TRUTH-1: unregistered + undocumented
                            # + unallowlisted, so it silently no-opped through
                            # this runner. Now registered (RendererFeatureRegistry.h)
                            # and documented (docs/tier1_env_vars.md); allowlisted
                            # here so setting it actually reaches mc2.exe.
                            "MC2_LIGHTING_DEBUG_VIEW",
                            # PERF-HARNESS-TRUTH-1: found by the new
                            # scripts/check-harness-gate-drive.py ratchet on its
                            # first run. tools/run_nvidia_readiness_smoke.py sets
                            # both in its FUNCTIONAL_ENV and delegates to this
                            # runner, which stripped them -- so the NVIDIA
                            # readiness runbook's "live pass/path identity" and
                            # "indirect submitters" traces produced NO output and
                            # the absence read as "nothing to report".
                            "MC2_RENDER_FRAME_PLAN_TRACE",
                            "MC2_MDI_SUBMIT_TRACE",
                            # ENV-DROP-CLOSE-1. HONEST NOTE, because several
                            # comments above this line are not: these two rows
                            # (like every other os.environ-copying row here) are
                            # NO-OPS. smoke_lib/runner.py builds the child env
                            # from os.environ.copy(), so an ambient MC2_* already
                            # reaches mc2.exe whether or not it is named here.
                            # They are listed for convention parity only -- both
                            # are actively A/B'd, and their absence kept being
                            # re-reported as an "ENV_DROP_RISK / voided A/B" by
                            # tools/repo_intel/gate_status.py (now corrected).
                            # If the runner ever stops inheriting os.environ this
                            # tuple becomes load-bearing for real; that is what
                            # scripts/check-smoke-env-inheritance.py guards.
                            "MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE",
                            "MC2_STATIC_UPDATE_SKIP")},
            },
        )
        # SMOKE-GATE-GUARD-1 / HARNESS-HYGIENE-3: ENV-DROP guard, corrected.
        #
        # THE OLD DIAGNOSIS WAS WRONG. This guard used to flag every MC2_* var
        # that was set in the shell but absent from the env_extra "allowlist",
        # on the stated grounds that it would be "silently dropped by the Popen
        # env-replacement". Reader source says otherwise: smoke_lib/runner.py
        # builds the child env with os.environ.copy() and then .update()s it,
        # so ambient MC2_* vars DO reach mc2.exe. Nothing is stripped. The
        # allowlist rows that copy os.environ values into env_extra are no-ops.
        #
        # The cost of the wrong diagnosis was real: the guard hard-stopped
        # legitimate A/B runs (an OFF arm setting MC2_TERRAIN_LOD_CHUNK=0 is
        # exactly the thing you want to be able to do) while explaining itself
        # with a mechanism that does not exist.
        #
        # The guard now distinguishes two cases:
        #   (a) UNDRIVEABLE -- the runner genuinely overwrites, deletes, or
        #       supersedes the ambient value (runner.FORCED_ENV / POPPED_ENV /
        #       a conflicting env_extra). Setting it does nothing, so a PASS
        #       would grade a configuration that never executed. FAIL CLOSED.
        #   (b) DELIBERATE -- the var passes through untouched. That is a
        #       legitimate run configuration, not an error. RECORD it (so the
        #       manifest says what was actually measured) and proceed.
        _undriveable = _smoke_runner.undriveable_ambient(os.environ, cfg.env_extra)
        _ambient_gates = sorted(
            k for k in os.environ
            if k.startswith("MC2_") and k not in _undriveable
        )
        if _ambient_gates:
            # Case (b). Not an error -- but never silent: an unrecorded ambient
            # var is how three scrubbed gates rode into dozens of captures
            # (see scripts/mc2_env_pin.py).
            _env_manifest["ambient_mc2_env"] = {
                k: os.environ[k] for k in _ambient_gates
            }
            print(
                f"[runner] [ENV-DECLARED] {len(_ambient_gates)} ambient MC2_* "
                f"var(s) will reach mc2.exe and are recorded in the run "
                f"manifest: "
                + ", ".join(f"{k}={os.environ[k]}" for k in _ambient_gates),
                file=sys.stderr,
            )
        for _dk, (_amb, _eff, _why) in sorted(_undriveable.items()):
            print(
                f"[runner] [ENV-DROP] WARNING: {_dk}={_amb!r} is set in your "
                f"shell but the harness cannot DRIVE it -- {_why}; the engine "
                f"will see {_eff!r}.",
                file=sys.stderr,
            )
        # DETERMINISM-RATCHET-2: a WARNING is not enough, and a prior session is
        # the proof. A tier1 command has printed "PASS (5/5)" alongside a
        # DEPLOY_FINGERPRINT MISMATCH for six different lanes -- a stderr line
        # nobody reads is indistinguishable from no line at all, and the run
        # still ends in a green verdict that gets cited as evidence.
        #
        # So for the UNDRIVEABLE set only, refuse to start. Costs nothing on a
        # normal run: this fires only when a var is ACTUALLY set AND actually
        # cannot be delivered.
        if _undriveable and not getattr(args, "allow_env_drop", False):
            print(
                f"\n[runner] [ENV-DROP] FATAL: {len(_undriveable)} MC2_* var(s) "
                f"set in the environment cannot be DRIVEN by this harness: "
                f"{', '.join(sorted(_undriveable))}.\n"
                f"[runner] Refusing to run -- a PASS here would grade a "
                f"configuration that was never applied.\n"
                f"[runner] Fix: these are overwritten/deleted by "
                f"scripts/smoke_lib/runner.py (FORCED_ENV / POPPED_ENV) or by "
                f"this run's env_extra. Unset them, drive them via env_extra, "
                f"or pass --allow-env-drop if you genuinely want them ignored.",
                file=sys.stderr,
            )
            # sys.exit, NOT `return`: main() terminates via sys.exit() and its
            # return value is discarded by the `main()` call at module scope, so
            # a bare `return 2` prints FATAL and then exits 0 -- a red message
            # with a green exit code, which is the precise failure this ratchet
            # exists to prevent. (Caught by checking the exit code instead of
            # trusting the message.)
            sys.exit(2)

        # Clear the file-sink probe log next to mc2.exe before each mission
        # so each run captures its own probe events.  See gos_terrain_indirect
        # PROBE_LOG / RING_SINK in the engine: ring-buffer / cmd-patch /
        # overshoot / near-clip-w / recipe-spread tripwires.
        ring_sink_path = Path(cfg.exe[0]).resolve().parent / "ring_trace.log"
        try:
            ring_sink_path.unlink(missing_ok=True)
        except Exception:
            pass

        print(f"[runner] running {e.stem} (tier={tier} duration={duration})",
              file=sys.stderr)
        result = run_one(cfg)

        # Deploy-fingerprint scan of the captured log. A mismatch/absence sets
        # _fp_hard_fail, which clears `passed` at the end of main() unless
        # MC2_SMOKE_REQUIRE_FINGERPRINT=0 (see the _fp_require site).
        if _fingerprint is not None:
            try:
                _fp = _fingerprint.parse_fingerprint(result.stdout_text)
                _fp_lines, _fp_bad = _fingerprint.check_fingerprint(
                    _fp, _fp_expected_sha, repo_root=ROOT)
                _fp_hard_fail = _fp_hard_fail or _fp_bad
                for _l in _fp_lines:
                    print(f"[runner] {_l}", file=sys.stderr)
            except Exception as _fpe:
                # SMOKE-DEPLOY-TRAP-1: "check skipped" used to leave
                # _fp_hard_fail untouched, so a raising check produced a clean
                # PASS on an unverified exe -- the same lie as a silent
                # mismatch. An unverifiable exe is a failed verification.
                _fp_hard_fail = True
                print(f"[runner] [DEPLOY_FINGERPRINT] check FAILED (treated as "
                      f"mismatch): {_fpe!r}", file=sys.stderr)

        # SMOKE-CRASH-SILENT-EVIDENCE-1: on an external-termination / freeze
        # verdict, capture cheap environment evidence (exit code, event-log TDR /
        # AppError / WER hints, minidump presence, concurrent mc2.exe, GPU/driver,
        # phase, stdout+heartbeat tail) so the flake can be classified. Strictly
        # post-verdict and best-effort — never touches the verdict.
        if _crash_evidence is not None and (
                set(result.verdict.buckets) & _crash_evidence.EVIDENCE_BUCKETS):
            try:
                _crash_evidence.capture(
                    e.stem, result, Path(cfg.exe[0]), artifact_dir,
                    enum_procs_fn=_enum_mc2_processes)
                print(f"[runner] [CRASH_EVIDENCE] {e.stem}: wrote "
                      f"{e.stem}.crash_evidence.json (exit={result.exit_code} "
                      f"buckets={','.join(result.verdict.buckets)})", file=sys.stderr)
            except Exception as _cee:
                print(f"[runner] [CRASH_EVIDENCE] capture skipped: {_cee}",
                      file=sys.stderr)

        key = baselines.key(cfg.profile, e.stem, tier, duration)
        delta = baselines.destroys_delta(baseline_data, key, result.summary.destroys)

        rows.append(report.Row(stem=e.stem, verdict=result.verdict,
                               summary=result.summary, destroys_delta=delta or 0))

        if not result.verdict.passed or args.keep_logs:
            (artifact_dir / f"{e.stem}.log").write_text(result.stdout_text, encoding="utf-8", errors="replace")
            # Snapshot the probe file-sink alongside the regular log.  Survives
            # across runs (the per-mission unlink above resets it for next).
            try:
                if ring_sink_path.exists():
                    (artifact_dir / f"{e.stem}.ring_trace.log").write_text(
                        ring_sink_path.read_text(encoding="utf-8", errors="replace"),
                        encoding="utf-8", errors="replace")
            except Exception as exc:
                print(f"[runner] could not snapshot ring_trace.log: {exc}", file=sys.stderr)
        if args.baseline_update and result.verdict.passed:
            baseline_data.setdefault(key, {})["destroys"] = {
                "mean": result.summary.destroys, "stddev": 0, "samples": 1,
                "updated": timestamp,
            }
            baseline_data[key]["perf"] = {
                "avg_fps": result.summary.perf.avg_fps,
                "p1low_fps": result.summary.perf.p1low_fps,
                "peak_ms": result.summary.perf.peak_ms,
            }

        # VEG-SMOKE-FLOOR-1: check vegetation output contract when gate is active.
        # Only fires when MC2_VEGETATION_CARDS=1; never affects non-veg smoke runs.
        if _veg_floor_active:
            _veg = _parse_veg_summary(result.stdout_text)
            _veg_fails = _check_veg_floor(e.stem, _veg)
            for _vf in _veg_fails:
                print(f"[runner] {_vf}", file=sys.stderr)
                _veg_floor_failures.append(_vf)
            if _veg:
                print(
                    f"[runner] [VEG-SMOKE-FLOOR] {e.stem}: "
                    f"instance_count={_veg['instance_count']} "
                    f"draw_calls={_veg['draw_calls']} "
                    f"flush_reason={_veg['flush_reason']} "
                    f"{'PASS' if not _veg_fails else 'FAIL'}",
                    file=sys.stderr)
            else:
                print(f"[runner] [VEG-SMOKE-FLOOR] {e.stem}: VEG_SUMMARY absent",
                      file=sys.stderr)

        # META-FIX design F: order-liveness floor (fixture-armed runs only).
        if _order_liveness_active:
            _ol = _parse_order_liveness_summary(result.stdout_text)
            _ol_fails = _check_order_liveness_floor(e.stem, _ol)
            for _of in _ol_fails:
                print(f"[runner] {_of}", file=sys.stderr)
                _order_liveness_failures.append(_of)
            if _ol:
                print(
                    f"[runner] [ORDER-LIVENESS] {e.stem}: "
                    f"issued_move={_ol['issued_move']} moved={_ol['moved']} "
                    f"refused={_ol['refused']} stalled={_ol['stalled']} "
                    f"verdict={_ol['verdict']} "
                    f"{'PASS' if not _ol_fails else 'FAIL'}",
                    file=sys.stderr)
            else:
                print(f"[runner] [ORDER-LIVENESS] {e.stem}: "
                      f"ORDER_LIVENESS_SUMMARY absent", file=sys.stderr)

        # META-FIX design A: advisory liveness scan — when the coherence audit
        # was armed for this mission (tier force-set or operator env), the
        # process-exit summary line must appear; its absence means the audit
        # never ran (mission died pre-first-tick, or the gate was dropped).
        # WARN only — never touches the verdict (the VIOLATION marker bucket
        # is the red path).
        _coh_armed = bool(args.tier) or (
            os.environ.get("MC2_MOVER_COHERENCE_AUDIT") not in (None, "", "0"))
        if _coh_armed:
            if "[MOVER_COHERENCE] summary" in result.stdout_text:
                _coh_m = re.search(
                    r"\[MOVER_COHERENCE\] summary checked=(\d+) violations=(\d+) "
                    r"advisories=(\d+) audits=(\d+)", result.stdout_text)
                if _coh_m:
                    print(f"[runner] [MOVER-COHERENCE] {e.stem}: "
                          f"checked={_coh_m.group(1)} "
                          f"violations={_coh_m.group(2)} "
                          f"advisories={_coh_m.group(3)} "
                          f"audits={_coh_m.group(4)}", file=sys.stderr)
            else:
                print(f"[runner] [MOVER-COHERENCE] WARN {e.stem}: audit armed "
                      f"but no [MOVER_COHERENCE] summary line — audit never "
                      f"ran (advisory)", file=sys.stderr)

        if args.fail_fast and not result.verdict.passed:
            print(f"[runner] --fail-fast: stopping at {e.stem}", file=sys.stderr)
            break

        # 2s grace for PDB lock release before next spawn.
        import time as _t; _t.sleep(2)

    # SMOKE-DEPLOY-TRAP-1: collect RUN-LEVEL failures BEFORE rendering. The
    # report used to be written here, ahead of the fingerprint verdict below,
    # so report.md/report.json (and the get_latest_smoke_report() MCP tool that
    # serves them) recorded "result=PASS (5/5 passed)" for a run that exited 1
    # because it had smoked the wrong binary. The exit code was honest; the
    # artifact people actually quote was not -- and quoting the artifact is
    # exactly what the eight trapped lanes did.
    _run_failures: list[str] = []
    if _fp_require and _fp_hard_fail:
        _run_failures.append(
            "DEPLOY_FINGERPRINT: the smoked exe does not match this worktree "
            "HEAD (stale / wrong-lane / built-from-dirty). The mission results "
            "below were produced by a different binary and prove nothing about "
            "this commit.")

    md = report.render_markdown(rows, tier=args.tier or "adhoc",
                                 profile=args.profile, timestamp=timestamp,
                                 show_window=args.show_window,
                                 run_failures=_run_failures)
    (artifact_dir / "report.md").write_text(md, encoding="utf-8")
    _report_json = report.render_json(rows, tier=args.tier or "adhoc",
                                      profile=args.profile, timestamp=timestamp,
                                      show_window=args.show_window,
                                      run_failures=_run_failures)
    # HARNESS-HYGIENE-3: record the env the run actually executed under, so a
    # deliberate ambient gate is auditable after the fact instead of refused
    # up front. Always present (empty dict = shipped default configuration).
    if isinstance(_report_json, dict):
        _report_json["env_manifest"] = dict(_env_manifest)
    (artifact_dir / "report.json").write_text(
        json.dumps(_report_json, indent=2), encoding="utf-8")

    if args.baseline_update:
        baselines.save(BASELINE_PATH, baseline_data)

    sys.stdout.buffer.write(md.encode("utf-8"))
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()
    passed = all(r.verdict.passed for r in rows) and (menu_canary_rc in (None, 0))

    # VEG-SMOKE-FLOOR-1: promote veg floor failures to hard fail when active.
    # Only active when MC2_VEGETATION_CARDS=1; non-veg runs are never affected.
    if _veg_floor_active and _veg_floor_failures:
        print(f"[runner] [VEG-SMOKE-FLOOR] HARD FAIL: {len(_veg_floor_failures)} "
              f"vegetation floor check(s) failed", file=sys.stderr)
        for _vf in _veg_floor_failures:
            print(f"[runner]   {_vf}", file=sys.stderr)
        passed = False

    # META-FIX design F: promote order-liveness failures to hard fail when the
    # fixture gate is set. Non-fixture runs are never affected.
    if _order_liveness_active and _order_liveness_failures:
        print(f"[runner] [ORDER-LIVENESS] HARD FAIL: "
              f"{len(_order_liveness_failures)} order-liveness check(s) failed",
              file=sys.stderr)
        for _of in _order_liveness_failures:
            print(f"[runner]   {_of}", file=sys.stderr)
        passed = False

    # Fingerprint mismatch/absence => hard failure. This is ON BY DEFAULT
    # (DEPLOY-FINGERPRINT-DEFAULT-ON-1); MC2_SMOKE_REQUIRE_FINGERPRINT=0 opts
    # out for the narrow case of deliberately smoking an external prebuilt exe.
    # Enforced by scripts/check-smoke-fingerprint-honesty.py -- do not weaken
    # the default without updating that contract check.
    if _fp_require and _fp_hard_fail:
        print("[runner] [DEPLOY_FINGERPRINT] HARD FAIL: smoke ran an exe whose "
              "fingerprint does not match this worktree HEAD (stale / wrong-lane "
              "/ built-from-dirty). Deploy your build first, or set "
              "MC2_SMOKE_REQUIRE_FINGERPRINT=0 to intentionally test an external "
              "exe.", file=sys.stderr)
        passed = False

    # VISUAL advisory (S13+).  Summarises the most recent run_visual.py compare
    # report (zero engine cost -- it does NOT capture). Advisory by default:
    # never changes the exit code. MC2_SMOKE_REQUIRE_VISUAL=1 promotes a non-PASS
    # visual verdict to a hard fail (mirrors the DEPLOY_FINGERPRINT opt-in). Runs
    # BEFORE the cockpit hook so any hard-fail is reflected in the cockpit result.
    try:
        _vis_report = os.environ.get(
            "MC2_SMOKE_VISUAL_REPORT",
            str(Path(__file__).resolve().parent.parent /
                "tests" / "visual" / "compare" / "visual_compare_report.json"))
        if os.path.isfile(_vis_report):
            with open(_vis_report, "r", encoding="utf-8") as _vf:
                _vrep = json.load(_vf)
            _vverdict = str(_vrep.get("overall_verdict", "UNKNOWN"))
            _vsets = len(_vrep.get("sets", []) or [])
            _vwhen = _vrep.get("generated_utc", "?")
            _vline = (f"[visual-advisory] last compare: {_vverdict} "
                      f"({_vsets} sets) generated_utc={_vwhen}")
            if os.environ.get("MC2_SMOKE_REQUIRE_VISUAL") == "1" and _vverdict != "PASS":
                print(f"[runner] [VISUAL] HARD FAIL: visual verdict {_vverdict} and "
                      "MC2_SMOKE_REQUIRE_VISUAL=1", file=sys.stderr)
                passed = False
        else:
            _vline = ("[visual-advisory] no visual compare on record "
                      "(run scripts/run_visual.py compare to populate)")
        print(_vline, file=sys.stderr)
        with open(artifact_dir / "report.md", "a", encoding="utf-8") as _rf:
            _rf.write("\n" + _vline + "\n")
    except Exception as _vexc:  # noqa: BLE001  advisory must never break the gate
        print(f"[runner] [visual-advisory] skipped ({_vexc})", file=sys.stderr)

    # SMOKE-GATE-GUARD-1: --require-gate assertion.
    # Gate → dotted counter path in latest_render_state.json["frame_graph"].
    # Counter must be > 0 to prove the gate reached the child and did work.
    _REQUIRE_GATE_COUNTERS: dict[str, str] = {
        "MC2_FRAMEGRAPH_EXECUTOR": "executor_owned_passes",
        "MC2_FRAMEGRAPH_DRYRUN": "frame_graph_dryrun.frames",
    }
    if args.require_gate:
        try:
            _rg_dir_env = os.environ.get("MC2_DEBUG_STATE_DUMP_DIR", "")
            _rg_state_dir = (Path(_rg_dir_env)
                             if _rg_dir_env
                             else Path(args.exe).resolve().parent / "debug_state")
            _rg_latest = _rg_state_dir / "latest_render_state.json"
            if not _rg_latest.exists():
                print(f"[runner] [REQUIRE_GATE] FATAL: latest_render_state.json not found "
                      f"at {_rg_latest} -- cannot verify --require-gate counters. "
                      f"Set MC2_DEBUG_STATE_DUMP=1.", file=sys.stderr)
                passed = False
            else:
                _rg_ds = json.loads(_rg_latest.read_text(encoding="utf-8"))
                _rg_fg = _rg_ds.get("frame_graph", {})
                for _rg_gate in args.require_gate:
                    _rg_path = _REQUIRE_GATE_COUNTERS.get(_rg_gate)
                    if _rg_path is None:
                        print(f"[runner] [REQUIRE_GATE] WARNING: {_rg_gate} has no "
                              f"known proof-counter in _REQUIRE_GATE_COUNTERS; "
                              f"add it to scripts/run_smoke.py.", file=sys.stderr)
                        continue
                    # Resolve dotted path (e.g. "frame_graph_dryrun.frames")
                    _rg_val: object = _rg_fg
                    for _rg_part in _rg_path.split("."):
                        if isinstance(_rg_val, dict):
                            _rg_val = _rg_val.get(_rg_part, 0)
                        else:
                            _rg_val = 0
                            break
                    _rg_count = int(_rg_val) if _rg_val else 0
                    if _rg_count == 0:
                        print(f"[runner] FATAL: --require-gate {_rg_gate} but its "
                              f"dump counter frame_graph.{_rg_path}={_rg_count} "
                              f"(var did not reach child or did nothing).",
                              file=sys.stderr)
                        passed = False
                    else:
                        print(f"[runner] [REQUIRE_GATE] {_rg_gate}: "
                              f"frame_graph.{_rg_path}={_rg_count} -- PASS",
                              file=sys.stderr)
        except Exception as _rg_exc:
            print(f"[runner] [REQUIRE_GATE] check failed: {_rg_exc}", file=sys.stderr)
            passed = False

    # Diag-state check: verify V2 schema + shutdown dump + CONFIG trace event.
    # Advisory by default; MC2_SMOKE_REQUIRE_DIAG_STATE=1 = hard fail on any
    # failed check. Only runs when MC2_DEBUG_STATE_DUMP=1 was active.
    try:
        _diag_require = os.environ.get("MC2_SMOKE_REQUIRE_DIAG_STATE") == "1"
        _diag_dump_active = os.environ.get("MC2_DEBUG_STATE_DUMP") == "1"
        if _diag_dump_active:
            _diag_state_dir_env = os.environ.get("MC2_DEBUG_STATE_DUMP_DIR", "")
            _diag_state_dir = (Path(_diag_state_dir_env)
                               if _diag_state_dir_env
                               else Path(args.exe).resolve().parent / "debug_state")
            _diag_latest = _diag_state_dir / "latest_render_state.json"

            _diag_checks: list[str] = []  # one line per check: PASS/FAIL + detail
            _diag_ok = True

            # --- schema check ---
            if not _diag_latest.exists():
                _diag_checks.append("FAIL latest_render_state.json not found")
                _diag_ok = False
            else:
                try:
                    _ds = json.loads(_diag_latest.read_text(encoding="utf-8"))
                    _sv = _ds.get("schema_version")
                    _dk = _ds.get("dump_kind")
                    _sid = _ds.get("session_id", "")
                    _pid = _ds.get("pid")
                    _wat = _ds.get("written_at_epoch")
                    if _sv != 2:
                        _diag_checks.append(
                            f"FAIL schema_version={_sv!r} (expected 2)")
                        _diag_ok = False
                    else:
                        _diag_checks.append("PASS schema_version==2")
                    if _dk != "shutdown":
                        _diag_checks.append(
                            f"FAIL dump_kind={_dk!r} (expected 'shutdown' after exit)")
                        _diag_ok = False
                    else:
                        _diag_checks.append("PASS dump_kind=shutdown")
                    for _field, _val in (("session_id", _sid), ("pid", _pid),
                                         ("written_at_epoch", _wat)):
                        if _val is None or _val == "":
                            _diag_checks.append(f"FAIL {_field} missing/empty")
                            _diag_ok = False
                        else:
                            _diag_checks.append(f"PASS {_field} present")
                except Exception as _je:
                    _diag_checks.append(f"FAIL cannot parse latest_render_state.json: {_je}")
                    _diag_ok = False
                    _sid = ""

            # --- CONFIG event check (diagnostic_trace.jsonl) ---
            # Relative MC2_DIAGNOSTIC_TRACE_FILE resolves from the exe dir
            # (engine CWD), not the runner CWD.  Absolute paths pass through.
            _trace_env = os.environ.get("MC2_DIAGNOSTIC_TRACE_FILE", "")
            if _trace_env:
                _tp = Path(_trace_env)
                _trace_path = _tp if _tp.is_absolute() else Path(args.exe).resolve().parent / _tp
            else:
                _trace_path = _diag_state_dir / "diagnostic_trace.jsonl"
            if _trace_path.exists():
                try:
                    _trace_lines = _trace_path.read_text(
                        encoding="utf-8-sig", errors="replace").splitlines()
                    # utf-8-sig strips a leading BOM if present (guards against
                    # stale trace files written by the now-fixed _wfopen bug).
                    # Scan last 200 lines for a CONFIG startup event.
                    _cfg_found = False
                    for _tline in _trace_lines[-200:]:
                        try:
                            _te = json.loads(_tline)
                            if _te.get("tag") == "CONFIG" and "diagnostic_trace_initialized" in str(_te.get("data", {})):
                                _cfg_found = True
                                break
                        except Exception:
                            continue
                    if _cfg_found:
                        _diag_checks.append("PASS diagnostic_trace.jsonl has CONFIG startup event")
                    else:
                        _diag_checks.append("FAIL no CONFIG startup event in diagnostic_trace.jsonl")
                        _diag_ok = False
                except Exception as _te:
                    _diag_checks.append(f"FAIL cannot read diagnostic_trace.jsonl: {_te}")
                    _diag_ok = False
            else:
                _diag_checks.append(
                    f"WARN diagnostic_trace.jsonl not found at {_trace_path} "
                    f"(set MC2_DIAGNOSTIC_TRACE_FILE or MC2_DIAG_TAGS to enable)")

            _diag_verdict = "PASS" if _diag_ok else "FAIL"
            _diag_lines = [f"# Diag-state check: {_diag_verdict}",
                           ""] + [f"- {c}" for c in _diag_checks]
            _diag_report_text = "\n".join(_diag_lines) + "\n"
            (artifact_dir / "diag_state_report.md").write_text(
                _diag_report_text, encoding="utf-8")

            _diag_summary = (f"[diag-state] {_diag_verdict}: "
                             + "; ".join(_diag_checks))
            if _diag_require and not _diag_ok:
                print(f"[runner] [DIAG_STATE] HARD FAIL: schema/trace check "
                      f"failed and MC2_SMOKE_REQUIRE_DIAG_STATE=1",
                      file=sys.stderr)
                passed = False
            print(_diag_summary, file=sys.stderr)
            with open(artifact_dir / "report.md", "a", encoding="utf-8") as _rf:
                _rf.write("\n" + _diag_summary + "\n")
    except Exception as _diag_exc:  # noqa: BLE001  advisory must never break the gate
        print(f"[runner] [diag-state] skipped ({_diag_exc})", file=sys.stderr)

    # Post-verdict cockpit hook (S2).  Verdict is already frozen above.
    # Any exception here is swallowed by cockpit.write_cockpit_artifacts;
    # it never changes the exit code.
    if _cockpit is not None:
        _cockpit.write_cockpit_artifacts(
            artifact_dir,
            exe_path=args.exe,
            tier=args.tier or "adhoc",
            profile=args.profile,
            missions=[r.stem for r in rows],
            durations={r.stem: (args.duration or 120) for r in rows},
            result="PASS" if passed else "FAIL",
            source="smoke",
        )

    # Release the deploy-folder lease (success OR failure path).
    # The atexit handler registered above is the backstop for os._exit / signals;
    # this explicit release fires first on normal exits and any exception that
    # propagates out of main().
    if _lease_folder is not None and _lease_available:
        try:
            _deploy_lease.release_lease(_lease_folder)
            _deploy_lease._log(f"[LEASE] released {_deploy_lease._short_name(_lease_folder)}")
            _lease_folder = None  # prevent double-release by atexit
        except Exception as _rel_exc:
            print(f"[runner] [LEASE] release failed (non-fatal): {_rel_exc}",
                  file=sys.stderr)

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()

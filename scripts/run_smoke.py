#!/usr/bin/env python3
# scripts/run_smoke.py
"""MC2 smoke matrix runner.

Examples:
  python scripts/run_smoke.py --tier tier1 --fail-fast
  python scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
  python scripts/run_smoke.py --tier tier2
  python scripts/run_smoke.py --tier resolution --deploy lane2    # weekly: one mission at 1920x1080 and 3840x2160
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


def _smoke_passthrough_names() -> frozenset:
    """Gate names whose table row carries smokePassthrough (Gates/gate_table.json)."""
    with open(ROOT / "Gates" / "gate_table.json", encoding="utf-8") as fh:
        return frozenset(g["name"] for g in json.load(fh)["gates"] if g.get("smokePassthrough"))


_SMOKE_PASSTHROUGH = _smoke_passthrough_names()

from scripts.smoke_lib import baselines, manifest, nofocus, render_options_pin, render_resolution, report
from scripts.smoke_lib import exe_guard as _exe_guard
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
ARTIFACT_ROOT = Path(os.environ.get("MC2_SMOKE_ARTIFACT_ROOT", ROOT / "tests" / "smoke" / "artifacts"))
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
    if sys.platform == "linux":
        from scripts.smoke_lib import child_registry
        rows = []
        for pid, _name in child_registry.engine_processes(images={"mc2"}):
            try:
                command = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
                rows.append((pid, child_registry.image_path(pid) or "", command))
            except OSError:
                continue
        return rows
    ps = (
        "Get-CimInstance Win32_Process -Filter \"Name='mc2.exe'\" | "
        "ForEach-Object { "
        "[pscustomobject]@{pid=$_.ProcessId;path=$_.ExecutablePath;cmd=$_.CommandLine} } | "
        "ConvertTo-Json -Compress"
    )
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
            text=True, stderr=subprocess.DEVNULL,
            **nofocus.hidden_kwargs())
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


def _menu_canary_refusal() -> str | None:
    """The canary replays Escape/Enter keystrokes as global input to the foreground window."""
    if nofocus.allow_foreground():
        return None
    return ("the menu canary sends synthetic keystrokes to whatever window has focus, which in an agent "
            "session is the owner's. It runs only in a user-present session: set "
            "MC2_HARNESS_ALLOW_FOREGROUND=1.")


def _run_menu_canary(exe: Path, script_path: Path, artifact_dir: Path,
                     keep_logs: bool, settle_s: int) -> int:
    why = _menu_canary_refusal()
    if why:
        print(f"[runner] ERROR: {why}", file=sys.stderr)
        return 5
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
        **nofocus.hidden_kwargs()
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


# Owner rule (game runs are about 15 s of in-mission time): a run with no --duration and no tier-declared duration gets this.
DEFAULT_DURATION_S = 15
# A freshly deployed exe rebuilds the model cache on its first launch (~85 s measured 2026-10-27): the load cap gets this much extra once.
COLD_CACHE_EXTRA_GRACE_S = 120


def cold_cache_extra_grace(exe, extra=COLD_CACHE_EXTRA_GRACE_S):
    """Extra load grace (s) when this exe has not been launched by the harness since it was deployed; records the launch."""
    exe = Path(exe)
    marker = exe.parent / ".smoke_warm"
    try:
        st = exe.stat()
        stamp = f"{st.st_size}:{st.st_mtime_ns}"
        cold = not marker.is_file() or marker.read_text().strip() != stamp
        if cold:
            marker.write_text(stamp)
    except OSError:
        return 0
    if cold:
        print(f"[runner] first launch of this exe since deploy: load grace +{extra}s for the model cache rebuild")
    return extra if cold else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["tier1", "tier2", "tier3", "resolution"])
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--objective-logistics", action="store_true",
                    help="initialize and settle logistics for the objectives_e6e fixtures")
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
    # of deploy path, blocks the run at once). Default is path-aware here and the
    # host-wide lease then waits for any foreign engine.
    ap.add_argument("--block-any-mc2", action="store_true",
                    help="block on any running mc2.exe regardless of its deploy "
                         "path (legacy global-block behavior; default is "
                         "path-aware)")
    ap.add_argument("--no-wait", action="store_true",
                    help="fail at once (exit 4) when another lane's engine is running or holds the host game lease; the default is "
                         "to wait for it (the lease is host-wide and covers every install)")
    ap.add_argument("--wait-timeout", type=float, default=None, metavar="SECONDS",
                    help="how long to wait for the host game lease (default 3600, or MC2_HOST_LEASE_TIMEOUT)")
    ap.add_argument("--duration", type=int)
    ap.add_argument("--runtime-bridge", action="store_true", help="dev-only loopback observation socket v1")
    ap.add_argument("--headless", action="store_true", help="no window, GPU device or audio (needs --ticks and the fixed clock; see docs/sim/headless-load.md)")
    ap.add_argument("--ticks", type=int, default=0,
                    help="tick-budget exit: quit after N sim ticks (needs MC2_SMOKE_FIXED_TIMESTEP=1, as the determinism runs set); --duration stays the timeout")
    ap.add_argument("--sim-determinism", action="store_true",
                    help="set only by tools/sim_determinism.py: raises the --duration cap from 30 to 60 s (owner ruling 2026-10-13: determinism pairs "
                         "need the longer window; tier smokes and captures keep the 30 s cap)")
    ap.add_argument("--sim-speed", type=int, choices=(1, 2, 3, 4), default=None, metavar="N",
                    help="non-visual proofs only (sim_determinism, saveload_roundtrip): MC2_SIM_SPEED=N runs N fixed 1/30 s sim steps per drawn "
                         "frame under the fixed-step clock, so the same sim frames take about 1/N of the host time; step size and RNG draws are those of 1x")
    ap.add_argument("--lite", action="store_true",
                    help="SMOKE-LITE-1 inner-loop profile: mc2_10 (night/urban, "
                         "heaviest) + mc2_01 (day) at --duration 10 (override "
                         "with --duration). ALL gates stay on (crash, heartbeat, "
                         "shader_error, deploy fingerprint, ...). "
                         "Reduced soak coverage: run full --tier tier1 before "
                         "milestones/landing.")
    ap.add_argument("--gpu", choices=("auto", "xtx", "2070"), default=None,
                    help="GPU routing: pass/fail runs default to the RTX 2070 SUPER (auto); perf/timing runs "
                         "(MC2_VK_GPU_TIMESTAMPS, --show-window) keep the RX 7900 XTX. Override with xtx or 2070")
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--content-error-budget", type=int, default=0,
                    help="maximum content errors for a mod smoke (stock default: zero)")
    ap.add_argument("--show-window", action="store_true",
                    help="Start mission windows visible instead of minimized; use for FPS/perf runs.")
    ap.add_argument("--exe", default=None,
                    help="explicit path to mc2.exe; skips auto-selection but still "
                         "leases the folder (use --no-lease to bypass leasing). "
                         "Use this for lane installs (E:/mc2-installs/<branch>/mc2.exe); "
                         "--deploy accepts only the short names")
    ref = ap.add_mutually_exclusive_group()
    ref.add_argument("--ref-commit", metavar="SHA",
                     help="fingerprint reference: a clean git commit SHA; default is HEAD")
    ref.add_argument("--ref-exe", metavar="PATH",
                     help="fingerprint reference: require SHA-256 byte identity with this executable")
    # Deploy-folder lease / checkout system (scripts/smoke_lib/deploy_lease.py).
    # --deploy <name>: request a specific folder by short name (0.4/0.4c/0.4d-rc1/
    #   0.5.0/0.5-testing). Error if that folder is busy-and-fresh.
    # --no-lease: skip the lease system entirely (no coordination, legacy behavior).
    ap.add_argument("--deploy",
                    metavar="NAME",
                    help="request a specific deploy folder by short name "
                         "(0.4 / 0.4c / 0.4d-rc1 / 0.5.0 / 0.5-testing; NOT a path - "
                         "lane installs use --exe <path>/mc2.exe); "
                         "waits (see --wait-timeout) while another run holds it")
    ap.add_argument("--no-lease", action="store_true",
                    help="OWNER ONLY (requires MC2_OWNER_NO_LEASE=1): skip the "
                         "deploy-folder lease system entirely. Agents never use "
                         "this; a stale lease is cleared with "
                         "`py -3 -m scripts.smoke_lib.deploy_lease --clear-dead`")
    # Tier 1.2 (docs/testing-strategy.md): opt-in safety net that promotes
    # GL_DEBUG_SEVERITY_HIGH from a silent log to abort(). Off by default.
    ap.add_argument("--resolve-only", action="store_true",
                    help="resolve and vet the install (lease, exe guard), print the "
                         "[SMOKE] exe=... src_commit=... line, then exit without "
                         "launching anything")
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
    ap.add_argument("--capture-bookmark", type=Path, action="append", default=[],
                    help="capture this saved camera during its matching mission; repeat for day/night")
    ap.add_argument("--capture-out", type=Path)
    ap.add_argument("--quiet-sample", help="mission to run without optional diagnostic/audit gates")
    ap.add_argument("--legacy-env", action="store_true",
                    help="A/B against an OLDER exe: forward only the MC2_* gates in Gates/gate_table.json at --ref-commit (the older exe's "
                         "commit), dropping the rest with a [LEGACY-ENV] line, so its strict config does not reject newer gates")
    args = ap.parse_args()
    if args.content_error_budget < 0:
        ap.error("--content-error-budget must be non-negative")
    if args.legacy_env:
        _legacy_ref = args.ref_commit or "HEAD"
        _t = subprocess.run(["git", "-C", str(ROOT), "show", f"{_legacy_ref}:Gates/gate_table.json"], capture_output=True, text=True)  # background-launch: allow git show, console tool
        if _t.returncode != 0:
            sys.exit(f"--legacy-env: no Gates/gate_table.json at {_legacy_ref} (pass --ref-commit SHA of the older exe's commit)")
        _smoke_runner.set_legacy_gates(_smoke_runner.legacy_gate_names(_t.stdout))
    if args.gpu:
        os.environ["MC2_HARNESS_GPU"] = args.gpu
    if args.capture_bookmark and not args.capture_out:
        ap.error("--capture-bookmark requires --capture-out")
    _fp_require = os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1") != "0"
    _fp_reference = None
    if _fingerprint is not None:
        try:
            _fp_reference = _fingerprint.resolve_reference(
                ROOT, ref_commit=args.ref_commit, ref_exe=args.ref_exe)
        except ValueError as exc:
            if _fp_require or args.ref_commit is not None or args.ref_exe is not None:
                ap.error(str(exc))
            print(f"[runner] [DEPLOY_FINGERPRINT] HEAD unavailable ({exc}) and "
                  "MC2_SMOKE_REQUIRE_FINGERPRINT=0 -- exe identity is NOT verified "
                  "for this run.", file=sys.stderr)
        if _fp_reference is not None:
            print("[runner] [DEPLOY_FINGERPRINT] reference: "
                  + _fingerprint.describe_reference(_fp_reference), file=sys.stderr)
    elif args.ref_commit is not None or args.ref_exe is not None:
        ap.error("fingerprint module unavailable; cannot verify a reference override")
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

    # SMOKE-LATENCY-WINS-1 win #1: --duration hard cap + quick-tier banner.
    # CLAUDE.md rule: NEVER --duration > 30 (soak/parity tier is 30s fixed); the one exception is a sim_determinism pair, which may run 60 (owner 2026-10-13).
    # 15s is a valid opt-in "does it still boot+run+not crash" inner-loop tier
    # (see .claude/SMOKE-LATENCY-1-RECON.md §4) -- all duration-independent
    # gates (crash_silent, crash_no_summary, engine_reported_fail,
    # pool_null, asset_oob, shader_error, missing_file) and the play-heartbeat
    # freeze gate (3s threshold) are satisfied well within 15s. It intentionally
    # loses soak coverage (mid-mission combat/FX/leak/hitch observation), so it
    # must stay opt-in and never silently replace the 30s default.
    duration_cap = 60 if args.sim_determinism else 30
    # Mission test runs explicitly request a simulation budget in minutes.
    # Keep longer wall budgets confined to this headless developer fixture.
    if args.headless and os.environ.get("MC2_PLAY_MISSION") == "1":
        duration_cap = max(duration_cap, int(os.environ.get("MC2_PLAY_MISSION_SECONDS", "300")) + 30)
    if args.duration is not None:
        if args.duration > duration_cap:
            print(f"[runner] ERROR: --duration {args.duration} exceeds the hard "
                  f"cap of {duration_cap}s. The 30s window is the soak/parity tier and must "
                  f"never be exceeded (CLAUDE.md); only tools/sim_determinism.py "
                  f"may run up to 60s (--sim-determinism). Use the default (30) or a "
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

    if args.no_lease and os.environ.get("MC2_OWNER_NO_LEASE") != "1":
        print("[runner] ERROR: --no-lease is owner-only and MC2_OWNER_NO_LEASE=1 is not set. "
              "Skipping the lease lets two smokes run on the host at once, which is what "
              "stole the owner's desktop and mouse on 2026-10-10. If a lease is stuck "
              "behind a dead run, clear it with "
              "`py -3 -m scripts.smoke_lib.deploy_lease --clear-dead` (only dead-owner "
              "leases are removed); if it is live, wait for that smoke to finish.",
              file=sys.stderr)
        sys.exit(6)
    if args.tier == "tier1" and not (args.exe or args.deploy):
        print("[runner] ERROR: --tier tier1 needs an explicit install: pass --deploy <name> "
              "(normally --deploy 0.4) or --exe <path>/mc2.exe. The runner never picks an "
              "install for a milestone tier on its own.", file=sys.stderr)
        sys.exit(2)

    try:
        from scripts.smoke_lib import deploy_lease as _deploy_lease
        _lease_available = True
    except Exception as _le:
        print(f"[runner] [LEASE] deploy_lease import failed: {_le}; "
              f"proceeding without leasing (--no-lease behavior)", file=sys.stderr)
        _lease_available = False

    # ----- Exe guard: never launch an exe that cannot stay off the desktop ----
    # Runs BEFORE any lease is taken, so a refused exe never holds a folder, and its first
    # line names the exe and the commit compiled into it. --verify-only launches nothing
    # and may inspect any install.
    if not args.verify_only:
        _pre_exe = args.exe
        if _pre_exe is None and _lease_available:
            _names = dict(_deploy_lease.DEPLOY_FOLDERS)
            _lane = args.deploy or _deploy_lease._WALK_LANES[0]
            if _lane in _names:
                _pre_exe = str(Path(_names[_lane]) / "mc2.exe")
        if _pre_exe is not None and Path(_pre_exe).exists():
            _exe_verdict = _exe_guard.check_exe(_pre_exe, purpose="smoke")
            print(_exe_verdict.banner(), file=sys.stderr, flush=True)
            if not _exe_verdict.ok:
                print(_exe_verdict.refusal(), file=sys.stderr, flush=True)
                sys.exit(7)

    # Validate every argument BEFORE the host-lease wait (a bad --deploy/--exe/--mission used
    # to fail only after up to an hour in the queue).
    if not args.verify_only:
        if args.deploy and _lease_available and args.deploy not in dict(_deploy_lease.DEPLOY_FOLDERS):
            print(f"[runner] ERROR: --deploy takes only a short name "
                  f"({', '.join(n for n, _ in _deploy_lease.DEPLOY_FOLDERS)}), got '{args.deploy}'. "
                  f"For a lane install such as E:/mc2-installs/<branch> pass --exe <path>/mc2.exe.",
                  file=sys.stderr)
            sys.exit(2)
        if args.exe is not None and not Path(args.exe).is_file():
            print(f"[runner] ERROR: --exe {args.exe} is not a file (expected <install>/mc2.exe).",
                  file=sys.stderr)
            sys.exit(2)
        if args.mission and not args.menu_canary:
            _early_unknown = select_missions(manifest.parse_manifest(MANIFEST_PATH), args.mission)[1]
            if _early_unknown and args.allow_any_mission:
                _early_exe = args.exe
                if _early_exe is None and _lease_available:
                    _early_exe = str(Path(dict(_deploy_lease.DEPLOY_FOLDERS).get(
                        args.deploy or _deploy_lease._WALK_LANES[0], "")) / "mc2.exe")
                if _early_exe:
                    _md = Path(_early_exe).resolve().parent / "data" / "missions"
                    _early_unknown = [s for s in _early_unknown
                                      if not ((_md / f"{s}.fit").is_file() or (_md / f"{s}.pak").is_file())]
            if _early_unknown:
                print(f"[runner] ERROR: unknown mission name(s) {', '.join(_early_unknown)} "
                      f"not found in {MANIFEST_PATH}"
                      f"{' or under the install data/missions' if args.allow_any_mission else ''}.",
                      file=sys.stderr)
                sys.exit(2)

    # Host slots precede install leases so queued runs do not reserve an install.
    _host_wait: dict = {"lease_wait_s": 0.0, "host_load": None}
    # --headless never opens a window or a GPU device, so it needs no host game slot (the install lease below still serialises one install).
    if not (args.verify_only or args.resolve_only or args.headless):
        from scripts.smoke_lib import host_lease as _host_lease
        _t_lease = time.monotonic()
        try:
            _host_lease.acquire(f"run_smoke {args.deploy or args.exe or 'auto'}", wait=not args.no_wait, timeout_s=args.wait_timeout,
                                kill_orphans=args.kill_existing,
                                exclusive=True if args.show_window or args.tier == "resolution" else None)
        except _host_lease.HostLeaseBusy as _busy:
            print(f"[runner] ERROR: {_busy}", file=sys.stderr)
            sys.exit(4)
        _host_wait["lease_wait_s"] = round(time.monotonic() - _t_lease, 1)
        if _host_lease.redirected():
            _host_wait["host_load"] = {"checked": False, "note": "private host lease (test): the real host is not sampled"}
        elif os.environ.get("MC2_HOST_LEASE_EXCLUSIVE") == "1" or args.show_window or args.tier == "resolution":
            from scripts.smoke_lib import host_load as _host_load
            _host_wait["host_load"] = _host_load.wait_until_quiet(timeout_s=0.0 if args.no_wait else None)

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
        # A folder another run holds is waited for (visible countdown, same bound as the host game lease); --no-wait fails at once.
        _lease_wait_s = 0.0 if args.no_wait else (
            args.wait_timeout if args.wait_timeout is not None
            else float(os.environ.get("MC2_HOST_LEASE_TIMEOUT") or 3600.0))
        try:
            if args.deploy:
                # --deploy <name>: named folder, must exist; waits while another run holds it
                _lease_folder = _deploy_lease.auto_acquire(
                    explicit_folder=None,
                    deploy_name=args.deploy,
                    intended_test=_intended_test,
                    wait_s=_lease_wait_s,
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
                    wait_s=_lease_wait_s,
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

    if args.resolve_only:
        sys.exit(0)

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
            _v = subprocess.run(cmd, capture_output=True, text=True, timeout=300, **nofocus.hidden_kwargs())
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
                                   capture_output=True, text=True, timeout=10,
                                   **nofocus.hidden_kwargs())
            _gexp = _gsha.stdout.strip() if _gsha.returncode == 0 else None
            _gbr = subprocess.run(["git", "-C", str(ROOT), "rev-parse",
                                   "--abbrev-ref", "HEAD"],
                                  capture_output=True, text=True, timeout=10,
                                  **nofocus.hidden_kwargs())
            _gbranch = _gbr.stdout.strip() if _gbr.returncode == 0 else None
            _gcmd = [sys.executable,
                     str(ROOT / "scripts" / "check-deploy-target.py"),
                     str(Path(args.exe).resolve().parent),
                     "--exe-name", Path(args.exe).name]
            if _gexp:
                _gcmd += ["--expected-sha", _gexp]
            if _gbranch:
                _gcmd += ["--branch", _gbranch]
            _g = subprocess.run(_gcmd, capture_output=True, text=True, timeout=60, **nofocus.hidden_kwargs())
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
    _pfcache_path = Path(args.exe).resolve().parent / ".preflight_cache.json"
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
                capture_output=True, text=True, timeout=30,
                **nofocus.hidden_kwargs())
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
                capture_output=True, text=True, timeout=15,
                **nofocus.hidden_kwargs())
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
                capture_output=True, text=True,
                **nofocus.hidden_kwargs()).stdout
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
    # A foreign engine (another install, another lane, the owner's game) no longer runs beside this one: the host-wide lease below
    # waits until it is gone.
    if args.block_any_mc2:
        blockers = same_path + foreign
    else:
        blockers = same_path
    if blockers:
        block_pids = [p for p, _ in blockers]
        if args.kill_existing:
            # Only an orphan of a DEAD harness run is ended (child_registry): a game the owner or another
            # lane started on this install, or one whose runner is still alive, is never killed.
            from scripts.smoke_lib import child_registry as _children
            refused = []
            for _pid in block_pids:
                _killed, _why = _children.kill_if_orphan(_pid)
                print(f"[runner] mc2.exe PID {_pid}: {'killed, ' if _killed else 'left running, '}{_why}",
                      file=sys.stderr)
                if not _killed:
                    refused.append(_pid)
            if refused:
                print(f"[runner] ERROR: mc2.exe PIDs {refused} were not started by a dead smoke run; "
                      f"close them yourself.", file=sys.stderr)
                sys.exit(4)
        else:
            print(f"[runner] ERROR: mc2.exe already running (PIDs {block_pids}); "
                  f"--kill-existing ends only orphans of a dead smoke run.", file=sys.stderr)
            sys.exit(4)

    if args.menu_canary or args.with_menu_canary:
        _why = _menu_canary_refusal()
        if _why:
            print(f"[runner] ERROR: {_why}", file=sys.stderr)
            sys.exit(5)
    if args.menu_canary:
        if args.tier or args.mission:
            ap.error("--menu-canary cannot be combined with --tier/--mission")
        timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S-%f") + f"-{os.getpid()}"
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
    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S-%f") + f"-{os.getpid()}"
    artifact_dir = ARTIFACT_ROOT / timestamp
    artifact_dir.mkdir(parents=True, exist_ok=True)
    print(f"[runner] artifacts: {artifact_dir}", flush=True)

    menu_canary_rc = None
    if args.with_menu_canary:
        print("[runner] running menu canary", file=sys.stderr)
        menu_canary_rc = _run_menu_canary(Path(args.exe), Path(args.menu_script),
                                          artifact_dir, args.keep_logs, args.menu_settle)
        if menu_canary_rc != 0 and args.fail_fast:
            print("[runner] --fail-fast: stopping after menu canary failure", file=sys.stderr)
            sys.exit(menu_canary_rc)

    # The pinned reference keeps every mission's verdict attributable to one build.
    _fp_expected_sha = _fp_reference.get("sha") if _fp_reference else None
    _fp_checks = []
    _fp_hard_fail = False
    # DEPLOY-FINGERPRINT-DEFAULT-ON-1 (Truth-First arc): default ON (opt-OUT via
    # MC2_SMOKE_REQUIRE_FINGERPRINT=0). A smoke that ran a stale/wrong-lane exe
    # (fingerprint mismatch) must NOT be allowed to report PASS — that is exactly
    # the "passed on the wrong binary" failure this arc exists to kill. Escape
    # hatch preserved for intentionally running a pre-built external exe.
    # SMOKE-DEPLOY-TRAP-1: no fingerprint module => no way to tell which exe we
    # smoked. Refuse to start rather than run the missions and print a verdict
    # that cannot be attributed to a binary.
    if _fingerprint is None:
        if _fp_require:
            print(
                f"\n[runner] [DEPLOY_FINGERPRINT] FATAL: the fingerprint module "
                f"could not be imported ({_fingerprint_import_error!r}), so the "
                f"smoked exe cannot be verified against the selected reference.\n"
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
        _fp_reference_text = (_fingerprint.describe_reference(_fp_reference)
                              if _fp_reference is not None else "worktree HEAD (unavailable)")
    else:
        _fp_reference_text = "worktree HEAD (fingerprint module unavailable)"

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
    _resolution_failures: list[str] = []
    _gpu_failures: list[str] = []
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

    _cold_grace = cold_cache_extra_grace(args.exe)
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

        duration = args.duration or e.duration or DEFAULT_DURATION_S
        tier = args.tier or "adhoc"
        # Infantry loads lazily after tick 2404; a short wall-clock tier run
        # missed its sim-worker render API call even with the audit armed.
        finale_audit = args.tier == "tier1" and e.stem == "mc2_24"
        # The `resolution` tier runs one stem once per size, so its rows, logs and baseline keys carry the size.
        label = f"{e.stem}@{e.resolution}" if e.resolution else e.stem
        cfg = RunConfig(
            exe=[args.exe] + (["--objective-logistics"] if args.objective_logistics else []),
            profile=e.profile or args.profile,
            stem=e.stem,
            duration=duration,
            heartbeat_timeout_load_s=e.heartbeat_timeout_load or 60,
            heartbeat_timeout_play_s=e.heartbeat_timeout_play or 3,
            grace_s=120 + _cold_grace,  # cap = duration + grace and includes load; a cold mc2_03 load took 64 s beside other engines
            allow_asset_oob=e.allow_asset_oob,
            content_error_budget=args.content_error_budget,
            # META-FIX INFRA-3: per-mission marker-bucket downgrades from the
            # manifest (allow_asset_oob analog for registered marker rows).
            marker_overrides=e.marker_overrides,
            show_window=args.show_window,
            ticks=max(args.ticks, 2600) if finale_audit else args.ticks,
            headless=args.headless,
            env_extra={
                "MC2_SMOKE_SEED": "0xC0FFEE",
                # META-FIX design A: force the read-only mover-registry
                # coherence audit ON for tier runs (same spirit as
                # MC2_DEBUG_STATE_DUMP in the canonical command — shipping
                # default stays OFF). Any [MOVER_COHERENCE] VIOLATION line
                # red-flags the mission via the mover_coherence marker bucket.
                **({"MC2_MOVER_COHERENCE_AUDIT": "1"} if args.tier and e.stem != args.quiet_sample else {}),
                # G7b overlap audit (dev selector, shipped OFF): ON in tier1 and smoke-lite. It aborts on the first
                # [SIM_OVERLAP v1] VIOLATION, which the sim_overlap_violation marker bucket turns red.
                # A caller-set level (2 = render-owns fence, 3..52 = jitter) wins, so a lane can run the stronger audit.
                **({"MC2_SIM_OVERLAP_AUDIT": os.environ.get("MC2_SIM_OVERLAP_AUDIT") or "1"} if args.tier == "tier1" or args.lite else {}),
                **({"MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_SIM_SPEED": "4"} if finale_audit else {}),
                **({"MC2_SIM_SPEED": str(args.sim_speed)} if args.sim_speed else {}),
                **(render_resolution.tier_size_env(tier, e.resolution) if e.resolution or args.tier == "resolution" or args.show_window
                   else {"MC2_WIDTH": "854", "MC2_HEIGHT": "480"}),
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
                # Gates the table marks smokePassthrough (Gates/gate_table.cpp); the list is
                # generated from the table, never hand-kept here.
                **{k: v for k, v in os.environ.items() if k in _SMOKE_PASSTHROUGH},
            },
        )
        if args.lite:
            # W82: pan across cached props and abort if registry liveness regresses.
            cfg.env_extra.update(MC2_STATIC_STALE_DROP_FATAL="1",
                                 MC2_REGFLUSH_GUARD2="1", MC2_CAM_SMOOTH_TEST="1")
        if args.quiet_sample and e.stem != args.quiet_sample:
            cfg.env_extra.update(MC2_VULKAN_VALIDATION="1", MC2_DEBUG_STATE_DUMP="1", MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE")
        if args.capture_bookmark:
            from run_visual_capture import smoke_capture
            cfg.env_extra.update(smoke_capture(args.capture_bookmark, e.stem, args.capture_out))
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
        # Harness-only knobs (MC2_CURSOR_NO_PARK, ...) and typos are not gates: the runner does not forward them
        # (the engine runs under MC2_CONFIG_STRICT=1) and says so here.
        _unregistered = _smoke_runner.unregistered_ambient(os.environ, cfg.env_extra)
        if _unregistered:
            _env_manifest["dropped_unregistered_env"] = dict(_unregistered)
            print(
                f"[runner] [ENV-DROPPED] {len(_unregistered)} ambient MC2_* name(s) are not gates in "
                f"Gates/gate_table.json and are NOT forwarded to mc2.exe: "
                + ", ".join(f"{k}={v}" for k, v in sorted(_unregistered.items())),
                file=sys.stderr,
            )
        _ambient_gates = sorted(
            k for k in os.environ
            if k.startswith("MC2_") and k not in _undriveable and k not in _unregistered
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

        print(f"[runner] running {label} (tier={tier} duration={duration})",
              file=sys.stderr)
        if args.runtime_bridge or os.environ.get("MC2_BRIDGE") == "1":
            from scripts.smoke_lib.runtime_bridge import RuntimeBridge
            bridge_output = Path(cfg.exe[0]).resolve().parent / "debug_state/latest_runtime_bridge.json"
            with RuntimeBridge(bridge_output) as bridge:
                cfg.env_extra.update(bridge.environment)
                result = run_one(cfg)
            print(f"[runner] runtime bridge: {bridge_output}", file=sys.stderr)
        else:
            # Normal harness launches cannot inherit a dev socket selector.
            cfg.env_extra.update(MC2_BRIDGE="0", MC2_BRIDGE_PORT="0", MC2_BRIDGE_SESSION="")
            result = run_one(cfg)
        if e.stem == args.quiet_sample:
            (artifact_dir / "quiet_sample.json").write_text(json.dumps({
                "mission": e.stem, "env_extra": cfg.env_extra,
                "ambient": {k: v for k, v in os.environ.items() if k in (_smoke_runner.known_gate_names() or ())},
                "passed": result.verdict.passed,
            }), encoding="utf-8")
        if args.capture_bookmark:
            smoke_capture(args.capture_bookmark, e.stem, args.capture_out, verify=True, exe=cfg.exe[0])

        # Deploy-fingerprint scan of the captured log. A mismatch/absence sets
        # _fp_hard_fail, which clears `passed` at the end of main() unless
        # MC2_SMOKE_REQUIRE_FINGERPRINT=0 (see the _fp_require site).
        if _fingerprint is not None:
            try:
                _fp = _fingerprint.parse_fingerprint(result.stdout_text)
                _fp_lines, _fp_bad = _fingerprint.check_fingerprint(
                    _fp, _fp_expected_sha, repo_root=ROOT,
                    reference=_fp_reference, exe_path=args.exe)
                _fp_checks.append({"mission": label, "hard_fail": _fp_bad,
                                   "lines": _fp_lines})
                _fp_hard_fail = _fp_hard_fail or _fp_bad
                for _l in _fp_lines:
                    print(f"[runner] {_l}", file=sys.stderr)
            except Exception as _fpe:
                # SMOKE-DEPLOY-TRAP-1: "check skipped" used to leave
                # _fp_hard_fail untouched, so a raising check produced a clean
                # PASS on an unverified exe -- the same lie as a silent
                # mismatch. An unverifiable exe is a failed verification.
                _fp_hard_fail = True
                _fp_checks.append({"mission": label, "hard_fail": True,
                                   "lines": [f"DEPLOY_FINGERPRINT: check FAILED: {_fpe!r}"]})
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
                    label, result, Path(cfg.exe[0]), artifact_dir,
                    enum_procs_fn=_enum_mc2_processes)
                print(f"[runner] [CRASH_EVIDENCE] {label}: wrote "
                      f"{label}.crash_evidence.json (exit={result.exit_code} "
                      f"buckets={','.join(result.verdict.buckets)})", file=sys.stderr)
            except Exception as _cee:
                print(f"[runner] [CRASH_EVIDENCE] capture skipped: {_cee}",
                      file=sys.stderr)

        key = baselines.key(cfg.profile, label, tier, duration)
        delta = baselines.destroys_delta(baseline_data, key, result.summary.destroys)

        _asked = render_resolution.asked_size(tier, e.resolution, cfg.env_extra)
        if _asked:
            # The size must have reached the engine: a clamped or ignored request would make this tier green without testing the size.
            _asked_w, _asked_h = _asked
            if f"[RESOLUTION] Pre-window: {_asked_w}x{_asked_h} (from MC2_WIDTH/MC2_HEIGHT env)" not in result.stdout_text:
                _resolution_failures.append(f"{label}: the engine log has no '[RESOLUTION] Pre-window: {_asked_w}x{_asked_h}' line")
        # The GPU the run asked for must be the one the engine logged ([VK_PROFILE] device): routing that does not reach the device pick is red.
        _gpu_msg = nofocus.gpu_mismatch({**os.environ, **cfg.env_extra}, bool(args.show_window or os.environ.get("MC2_SMOKE_VISIBLE")),
                                        [p.device for p in result.summary.vk_profile], result.stdout_text)
        if _gpu_msg:
            _gpu_failures.append(f"{label}: {_gpu_msg}")
        # The options the engine saw: the exe's options.cfg plus the MC2_OPTIONS_OVERRIDE of this run's environment (the runner passes the
        # ambient environment through and applies cfg.env_extra last).
        _options = render_options_pin.record(Path(args.exe).resolve().parent, {**os.environ, **cfg.env_extra})
        rows.append(report.Row(stem=label, verdict=result.verdict,
                               summary=result.summary, destroys_delta=delta or 0,
                               render_options=_options["values"]))

        if not result.verdict.passed or args.keep_logs:
            _kept_log = (f"[runner] [DEPLOY_FINGERPRINT] reference: {_fp_reference_text}\n"
                         + result.stdout_text)
            if _fp_checks and _fp_checks[-1]["mission"] == label:
                _kept_log += "\n" + "\n".join(_fp_checks[-1]["lines"]) + "\n"
            (artifact_dir / f"{label}.log").write_text(_kept_log, encoding="utf-8", errors="replace")
            # Snapshot the probe file-sink alongside the regular log.  Survives
            # across runs (the per-mission unlink above resets it for next).
            try:
                if ring_sink_path.exists():
                    (artifact_dir / f"{label}.ring_trace.log").write_text(
                        ring_sink_path.read_text(encoding="utf-8", errors="replace"),
                        encoding="utf-8", errors="replace")
            except Exception as exc:
                print(f"[runner] could not snapshot ring_trace.log: {exc}", file=sys.stderr)
        if args.baseline_update and result.verdict.passed:
            baseline_data.setdefault(key, {})["destroys"] = {
                "mean": result.summary.destroys, "stddev": 0, "samples": 1,
                "updated": timestamp,
            }
            # An informational record for whoever reads tests/smoke/baselines.json (a perf comparison starts from these numbers): no grader reads it, so a
            # run is never red for being slower than this; only `destroys` above is compared.
            baseline_data[key]["perf"] = {
                "avg_fps": result.summary.perf.avg_fps,
                "p1low_fps": result.summary.perf.p1low_fps,
                "peak_ms": result.summary.perf.peak_ms,
                # The fps means this drawable size: a pinned tier is recorded at the pinned size, the others at whatever options.cfg said.
                "resolution": (lambda s: f"{s[0]}x{s[1]}" if s else "install options.cfg size")(render_resolution.asked_size(tier, e.resolution)),
            }

        # VEG-SMOKE-FLOOR-1: check vegetation output contract when gate is active.
        # Only fires when MC2_VEGETATION_CARDS=1; never affects non-veg smoke runs.
        if _veg_floor_active and _options["values"]["ShadowQuality"] != 0 and _options["values"].get("BaseQuality", 2) == 2:
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
            f"DEPLOY_FINGERPRINT: the smoked exe failed verification against "
            f"{_fp_reference_text}. The mission results do not verify that reference.")

    if _resolution_failures:
        _run_failures.append("RESOLUTION: " + "; ".join(_resolution_failures))
    if _gpu_failures:
        _run_failures.append("GPU: " + "; ".join(_gpu_failures))
    _fp_record = {"reference": _fp_reference, "description": _fp_reference_text,
                  "required": _fp_require, "hard_fail": _fp_hard_fail,
                  "checks": _fp_checks}
    md = report.render_markdown(rows, tier=args.tier or "adhoc",
                                 profile=args.profile, timestamp=timestamp,
                                 show_window=args.show_window,
                                 run_failures=_run_failures, fingerprint=_fp_record,
                                 host_wait=_host_wait)
    (artifact_dir / "report.md").write_text(md, encoding="utf-8")
    _report_json = report.render_json(rows, tier=args.tier or "adhoc",
                                      profile=args.profile, timestamp=timestamp,
                                      show_window=args.show_window,
                                      run_failures=_run_failures, fingerprint=_fp_record,
                                 host_wait=_host_wait)
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
              f"identity failed verification against {_fp_reference_text}.", file=sys.stderr)
        passed = False

    if _resolution_failures:
        print(f"[runner] [RESOLUTION] HARD FAIL: {'; '.join(_resolution_failures)}", file=sys.stderr)
        passed = False

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
        _diag_dump_active = os.environ.get("MC2_DEBUG_STATE_DUMP") == "1" or bool(args.quiet_sample)
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
                            elif _te.get("tag") == "DEVICE":
                                _dev = _te.get("data", {})
                                _dev = _dev.get("device") if isinstance(_dev, dict) else str(_dev)
                                _diag_checks[:] = [c for c in _diag_checks if not c.startswith("INFO gpu=")]  # trace may span runs: keep the last
                                _diag_checks.append(f"INFO gpu={_dev}")
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
            durations={r.stem: (args.duration or DEFAULT_DURATION_S) for r in rows},
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

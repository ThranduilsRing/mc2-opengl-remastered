#!/usr/bin/env python3
# scripts/run_editor_playtest_smoke.py
r"""End-to-end PLAYTEST smoke for the MC2 Mission Editor.

Drives the editor's `-playtest` CLI mode (editor/EditorMFC.cpp): the editor loads
a mission headless, then triggers the REAL one-click Playtest path
(EditorPlaytest::Start()) -- exactly what a user clicking the "Playtest in Game"
button does -- with the launched game in smoke mode so it auto-quits cleanly.
The editor then exits with a code that encodes the child's fate:

  editor rc 0  -> child exited 0           (PASS)
  editor rc 2  -> launch/bridge/save abort (Start never went Running)
  editor rc 3  -> child exited nonzero     (game crash / mission-content fail)
  editor rc 4  -> timeout (child never completed in the window)

The verdict is read from the machine-readable line the editor emits on
completion (mirrors run_smoke.py / run_editor_smoke.py: the app reports facts,
the runner judges):

  [ESMOKE v1] event=playtest exit=<childExit> log=<archivedLogPath> mod=<id-or-none>

editor-startup.log (opened "w" at process start, under the deploy dir) is the
reliable IPC channel for a WIN32-subsystem app; stdout is also captured via an
inherited pipe.

PER-INSTALL GUARD (run_smoke.py lock philosophy, scoped): the playtest spawns a
real mc2.exe child from ONE install dir (MC2_PLAYTEST_EXE, else
..\mc2-win64-v0.4\mc2.exe relative to the editor exe). Multiple mc2 smokes may
run concurrently from DIFFERENT installs; we only refuse when a live mc2.exe's
image path lives inside the SAME install dir this harness would launch from.

Usage:
  py -3 scripts/run_editor_playtest_smoke.py --mission <pak path> [--timeout SEC]
                                             [--exe PATH] [--keep-logs]
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke_lib import nofocus  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
DEFAULT_EXE = r"A:\Games\mc2-opengl\mc2-win64-v0.4\Mission Editor.exe"
ARTIFACT_ROOT = REPO / "tests" / "smoke" / "editor"

# Strong crash signatures (the editor trace is verbose; weak tokens false-positive).
CRASH_TOKENS = ("unhandled exception", "access violation", "stack overflow", "fatal error")


def _running_mc2() -> list[tuple[int, str]]:
    """(pid, image_path) of any live mc2.exe (path may be '' if unqueryable)."""
    # Primary: wmic gives pid + full image path in one shot.
    try:
        out = subprocess.check_output(
            ["wmic", "process", "where", "name='mc2.exe'",
             "get", "ProcessId,ExecutablePath", "/FORMAT:CSV"],
            text=True, stderr=subprocess.DEVNULL,
            **nofocus.hidden_kwargs())
        procs = []
        for line in out.splitlines():
            parts = line.strip().split(",")
            # CSV: Node,ExecutablePath,ProcessId
            if len(parts) >= 3 and parts[-1].isdigit():
                procs.append((int(parts[-1]), parts[1].strip()))
        return procs
    except Exception:
        pass
    # Fallback: PowerShell (wmic removed on newer Windows).
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "Get-Process mc2 -ErrorAction SilentlyContinue | "
             "ForEach-Object { \"$($_.Id)|$($_.Path)\" }"],
            text=True, stderr=subprocess.DEVNULL,
            **nofocus.hidden_kwargs())
        procs = []
        for line in out.splitlines():
            if "|" in line:
                pid_s, path = line.split("|", 1)
                if pid_s.strip().isdigit():
                    procs.append((int(pid_s.strip()), path.strip()))
        return procs
    except Exception:
        return []


def _norm_dir(p: str | Path) -> str:
    """Normalized directory key for install-dir comparison."""
    return os.path.normcase(os.path.normpath(str(p)))


def _playtest_game_dir(editor_exe: Path) -> Path:
    """Install dir of the game exe the editor's playtest would launch
    (mirrors EditorPlaytest::ResolveExe: MC2_PLAYTEST_EXE env override, else
    probes ..\\mc2-win64-v0.4\\mc2.exe then .\\mc2.exe relative to the editor
    cwd = deploy dir)."""
    override = os.environ.get("MC2_PLAYTEST_EXE")
    if override:
        return Path(override).resolve().parent
    deploy = editor_exe.parent
    for cand in (deploy.parent / "mc2-win64-v0.4" / "mc2.exe",
                 deploy / "mc2.exe"):
        if cand.exists():
            return cand.resolve().parent
    return (deploy.parent / "mc2-win64-v0.4").resolve()


def _conflicting_mc2(editor_exe: Path) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    """Split running mc2.exe processes into (conflicting, other) by whether
    their image path is inside the install dir this harness would launch from.
    Processes with unknown paths are treated as conflicting (safe default)."""
    game_dir = _norm_dir(_playtest_game_dir(editor_exe))
    conflicting, other = [], []
    for pid, path in _running_mc2():
        if path and _norm_dir(Path(path).parent) != game_dir:
            other.append((pid, path))
        else:
            conflicting.append((pid, path or "(path unavailable)"))
    return conflicting, other


def _parse_playtest(text: str) -> dict:
    """Pull the `[ESMOKE v1] event=playtest ...` k=v fields (last match wins)."""
    fields = {}
    for m in re.finditer(r"\[ESMOKE v1\] event=playtest (.+)", text):
        fields = {}
        for kv in m.group(1).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
    return fields


def main() -> int:
    ap = argparse.ArgumentParser(description="MC2 Mission Editor end-to-end playtest smoke")
    ap.add_argument("--mission", required=True,
                    help="path to the mission .pak the editor loads then playtests")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="path to Mission Editor.exe")
    ap.add_argument("--timeout", type=int, default=120,
                    help="editor -playtest-timeout-sec (and the process backstop margin)")
    ap.add_argument("--active-mod", default=None,
                    help="MC2_ACTIVE_MOD for the editor (default: auto-detected from the "
                         "mission path's mods/<id>/ component, so the editor mounts the "
                         "mod's appearance assets when loading)")
    ap.add_argument("--keep-logs", action="store_true",
                    help="copy editor-startup.log into the report dir")
    ap.add_argument("--minimized", dest="minimized", action="store_true", default=True)
    ap.add_argument("--no-minimized", dest="minimized", action="store_false")
    ap.add_argument("--assert-bridge", action="store_true",
                    help="fixture assertion (mc2_01 green-path): FAIL unless the run emitted "
                         ">0 [MOVER v2] lines, >=2 distinct team= values, AND a [SMOKE v1] "
                         "summary result=pass. Verifies the editor->game runtime bridge end "
                         "to end (movers reached the game + the smoke ran clean).")
    args = ap.parse_args()

    exe = Path(args.exe)
    deploy = exe.parent
    # Fixture: `stock:mc2_01` names a stock mission materialized loose in the install by
    # `run_editor_smoke.py --prepare-stock-scratch` (lane/scratch installs).
    if args.mission.lower().startswith("stock:"):
        args.mission = str(deploy / "data" / "missions" / (args.mission.split(":", 1)[1] + ".pak"))
    mission = Path(args.mission)
    # The playtest must launch the game from THIS install, not the ..\mc2-win64-v0.4 probe
    # EditorPlaytest::ResolveExe tries first (a lane install would otherwise test the wrong exe).
    if not os.environ.get("MC2_PLAYTEST_EXE") and (deploy / "mc2.exe").exists():
        os.environ["MC2_PLAYTEST_EXE"] = str(deploy / "mc2.exe")
    if not exe.exists():
        print(f"ERROR: editor exe not found: {exe}\n  Build EditRel + deploy-editor (DEPLOY=...0.4c) first.")
        return 2
    if not mission.exists():
        print(f"ERROR: mission pak not found: {mission}
  Stock missions live in mission.fst; materialize them with "
              f"`run_editor_smoke.py --exe <editor> --prepare-stock-scratch --case gen_map_basic` on a lane install, "
              f"then pass --mission stock:mc2_01.")
        return 2

    # PER-INSTALL GUARD: refuse only if a running mc2.exe lives in the SAME
    # install dir the playtest would launch from (concurrent smokes from other
    # worktrees/deploys are fine).
    conflicting, other = _conflicting_mc2(exe)
    if conflicting:
        plist = "\n".join(f"  pid {pid}: {path}" for pid, path in conflicting)
        print(f"ERROR: mc2.exe already running from this playtest's install dir "
              f"({_playtest_game_dir(exe)}); refusing to run (it would fight for "
              f"the deployed exe / mission bridge).\n{plist}\n  Close that game first.")
        return 3
    if other:
        plist = "; ".join(f"pid {pid} ({path})" for pid, path in other)
        print(f"[playtest] note: unrelated mc2.exe running from other installs, "
              f"proceeding: {plist}")

    log = deploy / "editor-startup.log"
    try:
        if log.exists():
            log.unlink()
    except OSError:
        pass

    # The editor backstop is the playtest timeout; give the OUTER process wait a
    # generous margin (editor warm frames + child launch + child duration + archive).
    proc_timeout = args.timeout + 120

    argv = [str(exe), "-playtest", f"-playtest-timeout-sec={args.timeout}",
            f"-mission={mission}", "-exit-on-load-fail"]
    env = dict(os.environ)
    env["MC2_EDITOR_TRACE"] = "1"

    # BRIDGE FIXTURE: run the smoke child in ACTIVE-sim mode so the scenario clock
    # advances and live movers tick -> the editor forwards this as gos_smoke's
    # `--smoke-active`. Passive smoke (default) freezes the scenario and emits no
    # [MOVER v2] telemetry, so the bridge assertions could never pass.
    if args.assert_bridge:
        env["MC2_SMOKE_ACTIVE"] = "1"

    # Mount the mod so the editor can load the mission's mech/object appearances.
    # Auto-detect mods/<id>/ in the mission path unless explicitly overridden.
    active_mod = args.active_mod
    if active_mod is None:
        parts = [p.lower() for p in mission.parts]
        if "mods" in parts:
            idx = parts.index("mods")
            if idx + 1 < len(mission.parts):
                active_mod = mission.parts[idx + 1]
    if active_mod:
        env["MC2_ACTIVE_MOD"] = active_mod
        print(f"[playtest] MC2_ACTIVE_MOD={active_mod}")

    # BACKGROUND-SMOKE-1: minimized + non-activating is now UNCONDITIONAL via
    # nofocus. --minimized remains only as a no-op alias; it is no longer what
    # decides whether this run steals focus. The env var nofocus also sets
    # (MC2_NO_CURSOR_GRAB=1) is inherited by the mc2.exe child the editor
    # spawns for the playtest, so the GAME window does not activate either.
    print(f"[playtest] launch editor: -mission {mission.name}  timeout={args.timeout}s"
          f"{'  [minimized]' if args.minimized else ''}")
    t0 = time.time()
    timed_out = False
    try:
        proc = nofocus.spawn(argv, cwd=str(deploy), env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=proc_timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            out, _ = proc.communicate()
        rc = proc.returncode
    except FileNotFoundError:
        print(f"ERROR: could not launch editor exe: {exe}")
        return 2
    dt = round(time.time() - t0, 1)

    logtext = log.read_text(errors="replace") if log.exists() else ""
    combined = (out or "") + "\n" + logtext
    pt = _parse_playtest(combined)

    # Verdict --------------------------------------------------------------------
    bucket = ""
    if timed_out:
        bucket = "process_timeout"   # editor never exited (its own backstop failed)
    elif not pt:
        bucket = "no_playtest_summary"
    else:
        status = pt.get("status", "")
        child_exit = pt.get("exit", "?")
        if status == "launch_abort":
            bucket = "launch_abort"
        elif status == "timeout":
            bucket = "playtest_timeout"
        elif child_exit != "0":
            bucket = f"child_nonzero:{child_exit}"
        else:
            low = combined.lower()
            for tok in CRASH_TOKENS:
                if tok in low:
                    bucket = f"crash_token:{tok}"
                    break

    passed = (bucket == "") and rc == 0

    # --- BRIDGE FIXTURE ASSERTIONS (--assert-bridge, mc2_01 green-path) -----------
    # Verify the editor->game runtime bridge carried real mover state into the game:
    # >0 [MOVER v2] lines, >=2 distinct team= values (player + OpFor), and a
    # [SMOKE v1] summary result=pass. The archived log is the source of truth; the
    # editor mirrors the child's stdout into it, and `combined` includes it.
    bridge = {}
    if args.assert_bridge:
        # The child's stdout (incl. [MOVER v2] / [SMOKE v1]) is captured by the
        # editor and written to the ARCHIVED log, not the editor's own stdout/
        # startup log. Read that file (path from the ESMOKE `log=` field); fall
        # back to `combined` if the archive path is unavailable.
        bridge_text = combined
        archived = pt.get("log", "")
        if archived and archived not in ("-", "none"):
            try:
                ap_ = Path(archived)
                if ap_.exists():
                    bridge_text = ap_.read_text(errors="replace")
            except OSError:
                pass
        # Version-tolerant: matches [MOVER v1] (legacy) and [MOVER v2] (brain/path).
        mover_lines = re.findall(r"^\[MOVER v\d+\] (.+)$", bridge_text, re.MULTILINE)
        teams = set()
        for ml in mover_lines:
            tm = re.search(r"(?:^|\s)team=(\S+)", ml)
            if tm:
                teams.add(tm.group(1))
        smoke_results = re.findall(
            r"\[SMOKE v1\] event=summary result=(\S+)", bridge_text)
        smoke_pass = bool(smoke_results) and smoke_results[-1] == "pass"
        bridge = {
            "mover_lines": len(mover_lines),
            "team_values": sorted(teams),
            "smoke_pass": smoke_pass,
        }
        failures = []
        if len(mover_lines) <= 0:
            failures.append("no [MOVER v2] lines (bridge produced no mover state)")
        if len(teams) < 2:
            failures.append(f"only {len(teams)} distinct team= value(s) (need >=2): "
                            f"{sorted(teams)}")
        if not smoke_pass:
            failures.append("[SMOKE v1] summary result!=pass")
        if failures:
            passed = False
            bucket = bucket or ("bridge_assert:" + "; ".join(failures))

    verdict = "PASS" if passed else "FAIL"

    # Report ---------------------------------------------------------------------
    ts = time.strftime("%Y-%m-%dT%H-%M-%S")
    rptdir = ARTIFACT_ROOT / f"playtest-{ts}"
    rptdir.mkdir(parents=True, exist_ok=True)

    esmoke_line = ""
    m = re.search(r"\[ESMOKE v1\] event=playtest .+", combined)
    if m:
        esmoke_line = m.group(0)

    # Child SMOKE summary: surface a fail verdict from the game's own summary
    # line as a WARNING when the exit code said pass (does NOT change verdict).
    smoke_warning = ""
    sm = None
    for sm in re.finditer(r"\[SMOKE v1\] event=summary result=(\S+)(?:\s+reason=(\S*))?",
                          combined):
        pass  # last match wins
    if sm and sm.group(1) == "fail" and passed:
        smoke_warning = (f"WARNING: child smoke summary reported result=fail "
                         f"(reason={sm.group(2) or '-'}) despite exit code 0")

    lines = [
        f"# Editor playtest smoke {ts}  result={verdict}",
        "",
        f"- mission: `{mission}`",
        f"- editor exit: {rc}",
        f"- child exit: {pt.get('exit', '-')}",
        f"- mod: {pt.get('mod', '-')}",
        f"- archived log: {pt.get('log', '-')}",
        f"- bucket: {bucket or '-'}",
        f"- wall seconds: {dt}",
    ]
    if args.assert_bridge:
        lines += [
            "",
            "## bridge assertions",
            "",
            f"- mover lines: {bridge.get('mover_lines', 0)}",
            f"- distinct team= values: {bridge.get('team_values', [])}",
            f"- SMOKE result=pass: {bridge.get('smoke_pass', False)}",
        ]
    if smoke_warning:
        lines += ["", f"**{smoke_warning}**"]
    lines += [
        "",
        "## ESMOKE line",
        "",
        "```",
        esmoke_line or "(none)",
        "```",
    ]
    report = "\n".join(lines) + "\n"
    (rptdir / "report.md").write_text(report, encoding="utf-8")
    if args.keep_logs and logtext:
        (rptdir / "editor-startup.log").write_text(logtext, encoding="utf-8", errors="replace")
    (rptdir / "editor-stdout.txt").write_text(out or "", encoding="utf-8", errors="replace")

    print("\n" + report)
    if args.assert_bridge:
        print(f"[bridge] mover_lines={bridge.get('mover_lines', 0)} "
              f"teams={bridge.get('team_values', [])} "
              f"smoke_pass={bridge.get('smoke_pass', False)} "
              f"-> {'OK' if (bucket == '' or not bucket.startswith('bridge_assert')) and passed else 'FAIL'}")
    if smoke_warning:
        print(smoke_warning)
    print(f"report: {rptdir / 'report.md'}")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())

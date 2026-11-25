#!/usr/bin/env python3
"""Run the player brain through the off-screen/headless mission harness."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from smoke_lib.paths import INSTALL_ROOT

DEFAULT_INSTALL = (INSTALL_ROOT + '/claude-player-brain-l1')


def parse_report(text):
    rows = {}
    verdict = None
    for line in text.splitlines():
        if "[MISSION_OBJECTIVES v1]" in line:
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
            rows[int(fields["id"])] = fields
        if "[MISSION_VERDICT v1]" in line:
            verdict = line[line.index("[MISSION_VERDICT v1]"):]
    return verdict, rows


def run_mission(mission, install, seconds, batch=False, invulnerable=False, sim_speed=1, goto=None, mod=None):
    env = dict(os.environ, MC2_PLAY_MISSION="1",
               MC2_PLAY_MISSION_SKIP_CINEMATICS="1", MC2_PLAY_MISSION_SECONDS=str(seconds),
               MC2_RNG_SEED="42", MC2_SMOKE_SEED="42", MC2_DETERMINISTIC_RNG="1",
               MC2_SMOKE_FIXED_TIMESTEP="1")
    env.pop("MC2_PLAY_MISSION_INVULNERABLE", None)
    if invulnerable:
        env["MC2_PLAY_MISSION_INVULNERABLE"] = "1"
        env["MC2_BRIDGE_MOVER_STATE"] = "1"
    if goto:
        env["MC2_PLAY_MISSION_GOTO"] = goto
    if mod:  # "NAME:deps:boot" -> explicit mod mission without touching smoke_missions.txt
        name, deps, boot = (mod.split(":") + ["", ""])[:3]
        env.update(MC2_ACTIVE_MOD=name, MC2_MOD_DEPS=deps, MC2_BOOT_TO_BAY=boot, MC2_MENU_CANARY_SKIP_INTRO="1")
    # The harness owns install leasing, process priority, timeouts and cleanup.
    cmd = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--exe",
           str(Path(install) / "mc2.exe"), "--mission", mission,
           "--headless", "--ticks", str((seconds + 2) * 30), "--sim-speed", str(sim_speed),
           "--duration", str(seconds + 30), "--keep-logs", "--wait-timeout", "1200"]
    if mod:
        cmd.append("--allow-any-mission")
    sys.path.insert(0, str(ROOT / "scripts"))
    import console_child
    result = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace",
                            **console_child.child_kwargs(redirected=True))
    text = result.stdout
    artifacts = re.findall(r"^\[runner\] artifacts: (.+)$", text, re.MULTILINE)
    if artifacts:
        log = Path(artifacts[-1].strip()) / (mission + ".log")
        if log.exists():
            text += "\n" + log.read_text(encoding="utf-8", errors="replace")
    verdict, rows = parse_report(text)
    clean = result.returncode == 0 or (result.returncode == 1
            and "[SMOKE v1] event=tick_budget" in text and "Crash Report" not in text)
    verdict = verdict or "[MISSION_VERDICT v1] verdict=ERROR reason=no_engine_verdict"
    if not clean:
        verdict = re.sub(r"verdict=\w+", "verdict=ERROR", verdict) + " reason=harness_failed"
    print(f"{verdict} mission={mission}", flush=True)
    if not batch:
        print("id  text_key  state    primary hidden")
        for ident, row in sorted(rows.items()):
            print(f"{ident:<3} {row['text_key']:<9} {row['state']:<8} {row['primary']:<7} {row['hidden']}")
        for path in artifacts:
            print("log:", Path(path.strip()) / (mission + ".log"))
    if not clean or "reason=no_engine_verdict" in verdict:
        print("\n".join(result.stdout.splitlines()[-8:]), file=sys.stderr)
    return "verdict=PASS " in verdict and clean


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    missions = ap.add_mutually_exclusive_group(required=True)
    missions.add_argument("--mission")
    missions.add_argument("--batch", nargs="+", help="mission names, space or comma separated; duplicates repeat runs")
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--minutes", type=float, default=5)
    # headless already runs uncapped; sim-speed only helps windowed runs
    ap.add_argument("--sim-speed", type=int, choices=(1, 2, 3, 4), default=1, help="fixed 1/30 s sim steps per drawn frame (1 = old behaviour)")
    ap.add_argument("--invulnerable", action="store_true", help="keep the player alive (tests objective completion, not brain skill)")
    ap.add_argument("--goto", metavar="X,Y", help="order the player lance to this world position (dev gate MC2_PLAY_MISSION_GOTO)")
    ap.add_argument("--mod", metavar="NAME:DEPS:BOOT", help="run an explicit mod mission, e.g. MC2X-DEE:mc2x-compat:campaign (bypasses the allow-list, which stays unchanged)")
    args = ap.parse_args(argv)
    if args.minutes <= 0 or not args.minutes < float("inf"):
        ap.error("--minutes must be finite and positive")
    names = [name for item in (args.batch or [args.mission]) for name in item.split(",") if name]
    if not names:
        ap.error("--batch must contain at least one mission")
    seconds = max(1, round(args.minutes * 60))
    passed = sum(run_mission(name, args.install, seconds, bool(args.batch), **({"invulnerable": True} if args.invulnerable else {}), **({"sim_speed": args.sim_speed} if args.sim_speed != 1 else {}), **({"goto": args.goto} if args.goto else {}), **({"mod": args.mod} if args.mod else {})) for name in names)
    if args.batch:
        print(f"[MISSION_BATCH v1] passed={passed} failed={len(names) - passed} total={len(names)}", flush=True)
    return 0 if passed == len(names) else 1


if __name__ == "__main__":
    raise SystemExit(main())

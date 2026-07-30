#!/usr/bin/env python3
"""SWEEP-RUNNER-1: resolver-driven cheat-mode campaign sweep with structured emit.

Usage: py -3 scripts/sweep_campaign.py <campaign_folder> [secs=300] [deploy_dir]

Resolves launch config via resolve_campaign_config.py (no folder-name guessing),
runs the cheat-mode soak with MC2_BRIDGE_MOVER_STATE=1, then emits:
  resolver_config {active_mod, deps, fit, expected_missions}
  result {missions_completed, status (campaign_complete|crash|timeout), crash_site, hp_summary}
and appends one line to <deploy>/crash_catalog.jsonl.
Kills ONLY the process it launched. Never touches a concurrent mc2.exe.
"""
import json, os, re, subprocess, sys, time, glob
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
# VERIFY-INTEGRITY-3: this harness launches mc2.exe for up to `secs` (default
# 300s) and previously set NEITHER MC2_SMOKE_MODE nor MC2_NO_CURSOR_GRAB, so the
# game clamped and re-centred the user's pointer for the whole soak. Belt and
# braces, matching scripts/smoke_lib/runner.py: the env gate stops NEW binaries
# grabbing at all, and CursorGuard defends from outside the process so OLDER
# deploys (this script targets v0.4d-rc1 by default) are covered too.
from smoke_lib.cursor_guard import CursorGuard  # noqa: E402
from smoke_lib import nofocus  # noqa: E402
# Computer-agnostic: override with MC2_DEPLOY_DIR env; literal is a last-resort dev fallback.
DEFAULT_DEPLOY = os.environ.get("MC2_DEPLOY_DIR", r"A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1")

def resolve(deploy, name):
    out = subprocess.run([sys.executable, str(HERE / "resolve_campaign_config.py"), deploy, name],
                         capture_output=True, text=True)
    return json.loads(out.stdout)

def newest_crash(deploy):
    dirs = sorted(glob.glob(os.path.join(deploy, "crashes", "crash_*")), key=os.path.getmtime)
    return dirs[-1] if dirs else None

def main():
    name = sys.argv[1]
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    deploy = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_DEPLOY
    cfg = resolve(deploy, name)
    exe = os.path.join(deploy, "mc2.exe")
    tag = re.sub(r"[^A-Za-z0-9]", "_", name)[:20]
    outp = os.path.join(deploy, f"sweep_{tag}_out.txt")
    errp = os.path.join(deploy, f"sweep_{tag}_err.txt")

    env = os.environ.copy()
    for k in ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS", "MC2_BOOT_TO_BAY"):
        env.pop(k, None)
    env["MC2_ACTIVE_MOD"] = cfg["active_mod"]
    env["MC2_MOD_DEPS"]   = cfg["mc2_mod_deps_string"]
    env["MC2_BOOT_TO_BAY"] = cfg["fit"]
    env.update({
        "MC2_SOAK_AUTOWIN": "1", "MC2_SOAK_WIN_AFTER_SEC": "3",
        "MC2_MENU_CANARY_SKIP_INTRO": "1", "MC2_LOG": "1",
        "MC2_SOAK_AUTO_PURCHASE": "1", "MC2_SOAK_KILL_ENEMY": "1",
        "MC2_SOAK_PILOT_PROMOTE": "1", "MC2_SOAK_CHECK_SCREENS": "1",
        "MC2_SOAK_LANCE_RANDOM": "1", "MC2_BRIDGE_MOVER_STATE": "1",
        "MC2_DEBUG_STATE_DUMP": "1", "MC2_DIAG_TAGS": "LOGISTICS",
        # VERIFY-INTEGRITY-3: engine-side cursor release (commit f0c07b87).
        # NOT MC2_SMOKE_MODE -- that changes game behaviour (skips movies,
        # pins the RNG seed) and would alter what this soak actually exercises.
        "MC2_NO_CURSOR_GRAB": "1",
    })
    crash_before = newest_crash(deploy)
    with open(outp, "w") as fo, open(errp, "w") as fe:
        # Guard STARTS BEFORE Popen so its anchor is the user's real position
        # and any foreign clamp is reported rather than silently adopted.
        with CursorGuard():
            p = nofocus.spawn([exe], cwd=deploy, stdout=fo, stderr=fe, env=env)
            try:
                p.wait(timeout=secs)
                exited = True
            except subprocess.TimeoutExpired:
                p.kill(); exited = False

    out = Path(outp).read_text(errors="replace")
    err = Path(errp).read_text(errors="replace")
    missions = len(re.findall(r"\[SOAK\] autowin mission=", out))
    complete = "campaign-complete" in out
    fast_exit = "FAST-EXIT" in err
    status = "campaign_complete" if complete else ("crash" if fast_exit else ("timeout" if not exited or not complete else "exit"))
    crash_site = None
    if fast_exit:
        cd = newest_crash(deploy)
        if cd and cd != crash_before:
            ct = Path(os.path.join(cd, "crash.txt")).read_text(errors="replace")
            m = re.search(r"#00 .*?(\w+::\w+|\w+)\s+\(([^)]+)\)", ct)
            fault = re.search(r"(READ|WRITE|EXEC) violation at (0x[0-9A-Fa-f]+)", ct)
            crash_site = {"frame00": m.group(0).strip() if m else "?",
                          "fault": fault.group(0) if fault else "?",
                          "bundle": os.path.basename(cd)}
    hps = re.findall(r"\bhp=([0-9.]+)", out)
    low = sum(1 for h in hps if float(h) < 0.5)
    hp_summary = {"mover_samples": len(hps), "low_hp_lt0.5": low,
                  "min_hp": min((float(h) for h in hps), default=None)}

    summary = {
        "campaign": name,
        "resolver_config": {"active_mod": cfg["active_mod"], "deps": cfg["mc2_mod_deps_string"],
                            "fit": cfg["fit"], "expected_missions": cfg.get("expected_missions")},
        "result": {"missions_completed": missions, "status": status,
                   "crash_site": crash_site, "hp_summary": hp_summary},
    }
    print(json.dumps(summary, indent=2))
    with open(os.path.join(deploy, "crash_catalog.jsonl"), "a") as cat:
        cat.write(json.dumps(summary) + "\n")

if __name__ == "__main__":
    main()

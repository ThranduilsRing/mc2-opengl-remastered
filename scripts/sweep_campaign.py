#!/usr/bin/env python3
"""SWEEP-RUNNER-1: resolver-driven cheat-mode campaign sweep with structured emit.

Usage: py -3 scripts/sweep_campaign.py <campaign_folder|stock> [secs=300] <deploy_dir> [--shots] [--salvage] [--liveness] [--out DIR]
  stock    = the stock campaign (fit "campaign", no mod); the resolver is skipped.
  deploy_dir is REQUIRED; the owner v0.4 / owner-test installs are refused unless --allow-owner-install.
  --shots  = MC2_SHOT_RESULTS + MC2_DEV_SHELL: mission-start, salvage and pilot-promotion screenshots per mission, copied to --out.
  --salvage= MC2_SOAK_SALVAGE_TRADEMOVER (real mid-mission salvage-craft flow).
  --out    = copy stdout/stderr/screenshots and write sweep_summary.json (+ per-mission [SOAK] counts) there.

Resolves launch config via resolve_campaign_config.py (no folder-name guessing),
runs the cheat-mode soak with MC2_BRIDGE_MOVER_STATE=1, then emits:
  resolver_config {active_mod, deps, fit, expected_missions}
  result {missions_completed, status (campaign_complete|crash|timeout), crash_site, hp_summary}
and appends one line to <deploy>/crash_catalog.jsonl.
Kills ONLY the process it launched. Never touches a concurrent mc2.exe.
"""
import json, os, re, shutil, subprocess, sys, time, glob
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
# MC2_SMOKE_MODE + MC2_NO_CURSOR_GRAB (set below) stop the game grabbing the pointer; nofocus.spawn refuses an exe too old to honour them.
from smoke_lib import nofocus  # noqa: E402

def resolve(deploy, name):
    out = subprocess.run([sys.executable, str(HERE / "resolve_campaign_config.py"), deploy, name],
                         capture_output=True, text=True,
                         **nofocus.hidden_kwargs())
    return json.loads(out.stdout)

def newest_crash(deploy):
    dirs = sorted(glob.glob(os.path.join(deploy, "crashes", "crash_*")), key=os.path.getmtime)
    return dirs[-1] if dirs else None

def main():
    args = sys.argv[1:]
    flags = {a for a in args if a in ("--shots", "--salvage", "--liveness", "--allow-owner-install")}
    out_dir = None
    sim_speed = 1   # headless already runs uncapped; sim-speed only helps windowed runs
    if "--sim-speed" in args:
        i = args.index("--sim-speed")
        sim_speed = int(args[i + 1])
        if not 1 <= sim_speed <= 4:
            sys.exit('sweep_campaign: --sim-speed must be 1-4 (got %d)' % sim_speed)
        del args[i:i + 2]
    if "--out" in args:
        i = args.index("--out")
        out_dir = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if a not in flags]
    name = args[0]
    secs = int(args[1]) if len(args) > 1 else 300
    if len(args) < 3:
        sys.exit("sweep_campaign: deploy_dir is required (use your own G:/mc2-installs/<lane>); there is no default install")  # drive-path: ok PC path in user-facing help
    deploy = args[2]
    if re.search(r"mc2-win64-v0\.4|owner-test", deploy.replace("\\", "/")) and "--allow-owner-install" not in flags:
        sys.exit("sweep_campaign: refusing the owner install %s (pass --allow-owner-install to override)" % deploy)
    if name == "stock":
        cfg = {"active_mod": "", "mc2_mod_deps_string": "", "fit": "campaign", "expected_missions": None}
    else:
        cfg = resolve(deploy, name)
    exe = os.path.join(deploy, "mc2.exe")
    tag = re.sub(r"[^A-Za-z0-9]", "_", name)[:20]
    outp = os.path.join(deploy, f"sweep_{tag}_out.txt")
    errp = os.path.join(deploy, f"sweep_{tag}_err.txt")

    env = os.environ.copy()
    for k in ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS", "MC2_BOOT_TO_BAY"):
        env.pop(k, None)
    if sim_speed > 1:   # N fixed 1/30 s steps per drawn frame (needs the fixed-step clock)
        env["MC2_SMOKE_FIXED_TIMESTEP"] = "1"
        env["MC2_SIM_SPEED"] = str(sim_speed)
    if cfg["active_mod"]:
        env["MC2_ACTIVE_MOD"] = cfg["active_mod"]
        env["MC2_MOD_DEPS"]   = cfg["mc2_mod_deps_string"]
    env["MC2_BOOT_TO_BAY"] = cfg["fit"]
    env.update({
        "MC2_SOAK_AUTOWIN": "1", "MC2_SOAK_WIN_AFTER_SEC": "3",
        "MC2_MENU_CANARY_SKIP_INTRO": "1", "MC2_LOG": "1",
        "MC2_SOAK_AUTO_PURCHASE": "1", "MC2_SOAK_KILL_ENEMY": "1",
        "MC2_SOAK_PILOT_PROMOTE": "1", "MC2_SOAK_CHECK_SCREENS": "1",
        "MC2_SOAK_LANCE_RANDOM": "1", "MC2_BRIDGE_MOVER_STATE": "1",
        "MC2_DEBUG_STATE_DUMP": "1", "MC2_DIAG_TAGS": os.environ.get("MC2_DIAG_TAGS", "LOGISTICS"),
        # VERIFY-INTEGRITY-3: engine-side cursor release (commit f0c07b87).
        # NOT MC2_SMOKE_MODE -- that changes game behaviour (skips movies,
        # pins the RNG seed) and would alter what this soak actually exercises.
        "MC2_NO_CURSOR_GRAB": "1",
    })
    if "--liveness" in flags:   # AI/movement sample: enemies alive, 40 s before auto-win (default walk kills them at 3 s)
        env["MC2_SOAK_WIN_AFTER_SEC"] = "40"
        env.pop("MC2_SOAK_KILL_ENEMY", None)
        env["MC2_FIXTURE_ORDER_LIVENESS"] = "1"   # player advance: attack order at 0.2, move at 0.5 of the window, so idle enemies get provoked
    if "--salvage" in flags:
        env["MC2_SOAK_SALVAGE_TRADEMOVER"] = "1"
    if "--shots" in flags:
        env.update({"MC2_SHOT_RESULTS": "1", "MC2_DEV_SHELL": "1", "MC2_NO_LAUNCHER": "1"})
    # Private save dir (engine MC2_SAVE_DIR): the walk completes missions and writes after_mission_*.fit; it must never touch the player's
    # savegame folder, which concurrent gate rows (player-path) watch.
    save_dir = os.path.join(deploy, f"sweep_{tag}_savegame")
    shutil.rmtree(save_dir, ignore_errors=True)
    os.makedirs(save_dir, exist_ok=True)
    env["MC2_SAVE_DIR"] = save_dir
    crash_before = newest_crash(deploy)
    shot_dir = os.path.join(deploy, "dev_shell_out")
    t0 = time.time()
    with open(outp, "w") as fo, open(errp, "w") as fe:
        p = nofocus.spawn([exe], cwd=deploy, stdout=fo, stderr=fe, env=env)
        try:
            p.wait(timeout=secs)
            exited = True
        except subprocess.TimeoutExpired:
            p.kill(); exited = False

    wall = time.time() - t0
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
    summary["result"]["wall_seconds"] = round(wall, 1)
    summary["result"]["exit_code"] = p.returncode
    summary["result"]["missing_tga"] = len(re.findall(r"missing (?:terrain|mask|overlay) tga", err))
    summary["result"]["soak_counts"] = {k: len(re.findall(r"\[SOAK\] " + k, out)) for k in
        ("autowin mission=", "results dismiss screen=salvage", "results dismiss screen=pilot", "salvage-check",
         "salvage-in-loadbay", "salvage-trademover", "pilot-promote", "kill-enemy")}
    summary["result"]["promotions_ok"] = len(re.findall(r"\[SOAK\] pilot-promote .*ok=1", out))
    summary["result"]["autowin_missions"] = re.findall(r"\[SOAK\] autowin mission=(\S+)", out)
    if "--liveness" in flags:
        import campaign_liveness
        summary["result"]["liveness"] = [r["summary"] for r in campaign_liveness.analyse(out)]
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        copied = []
        for s_ in glob.glob(os.path.join(shot_dir, "*.tga")):
            if os.path.getmtime(s_) >= t0:
                shutil.copy2(s_, out_dir)
                copied.append(os.path.join(out_dir, os.path.basename(s_)))
        summary["result"]["screenshots"] = sorted(copied)
        for f in (outp, errp):
            shutil.copy2(f, out_dir)
        Path(out_dir, "sweep_summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    with open(os.path.join(deploy, "crash_catalog.jsonl"), "a") as cat:
        cat.write(json.dumps(summary) + "\n")

if __name__ == "__main__":
    main()

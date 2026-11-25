import json, sys, time, os
from pathlib import Path
ROOT = Path("A:/Games/mc2-lane10")
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/perf"), str(ROOT / "scripts")]
import psutil
import mission_cpu_profile as mcp
import run_battle_bench as battle
from smoke_lib import host_lease, runner

from smoke_lib.paths import INSTALL_ROOT, PROOF_ROOT

EXE = Path((INSTALL_ROOT + '/claude-perf-counters/mc2.exe'))
OUT = Path((PROOF_ROOT + '/census2'))
VIEWS = {"mc2_01": "sun_facing_low", "mc2_10": "sun_facing_low", "mc2_24": "low_horizon_0"}


def run(mission, tag, extra_cmds, seconds, use_view=True, late_cmds=()):
    d = OUT / f"{mission}_{tag}"
    d.mkdir(parents=True, exist_ok=True)
    cmds = [f"bench_output {d / 'frames.jsonl'}"]
    if use_view and mission in VIEWS:
        v = next(m for m in mcp.camera_bookmarks(mission, True)["bookmarks"] if m["name"] == VIEWS[mission])
        cmds.append(battle.view_command(v))
    cmds += extra_cmds
    ch = d / "commands.txt"
    ch.write_text("\n".join(cmds) + "\n", encoding="utf-8")
    env = battle.environment(ch, 1920, 1080)
    import threading
    def late():
        ack = ch.with_suffix(ch.suffix + ".ack")
        while not (ack.exists() and "bench_view" in ack.read_text(errors="replace")):
            time.sleep(0.2)
        time.sleep(4)
        ch.write_text(chr(10).join(late_cmds) + chr(10), encoding="utf-8")
    if late_cmds:
        threading.Thread(target=late, daemon=True).start()
    samples = mcp.HostSamples(psutil)
    with host_lease.exclusive_window(f"census2 {mission} {tag}", timeout_s=1500):
        try:
            with samples:
                res = runner.run_one(runner.RunConfig(exe=[str(EXE)], profile="stock", stem=mission,
                    duration=seconds, heartbeat_timeout_load_s=180, heartbeat_timeout_play_s=15,
                    grace_s=150, env_extra=env, show_window=True))
        finally:
            noise = mcp.noise_report(samples)
    noise.pop("samples", None)
    (d / "engine.log").write_text(res.stdout_text, encoding="utf-8")
    (d / "noise.json").write_text(json.dumps(noise), encoding="utf-8")
    ok = res.exit_code == 0 and not res.killed_by_timeout and res.verdict.passed
    print(f"{mission} {tag} ok={ok} noise={noise['status']} ext_p95={noise['external_cpu_p95']} whole={noise['whole_machine_external_cpu_p95']}", flush=True)
    return ok


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "main":
        for mission in ["mc2_01", "mc2_03", "mc2_10", "mc2_17", "mc2_24"]:
            for i in range(5):
                run(mission, f"r{i}", [], 20)
    elif mode == "walk":
        for mission in ["mc2_24", "mc2_01"]:
            run(mission, "walk", ["bench_freeze on", "bench_walk 200"], 12)
    elif mode == "walk2":
        for mission in ["mc2_24", "mc2_01"]:
            run(mission, "walk2", [], 14, late_cmds=["bench_freeze on", "bench_walk 200"])
    elif mode == "one":
        run(sys.argv[2], "t", sys.argv[3:], 12)

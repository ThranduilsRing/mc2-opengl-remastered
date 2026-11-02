"""Census v3 driver + analyzer: worst-case (corner zoom-out) poses, 1080p and 4K, one lane install.

usage: py -3 tools/perf/_census_v3.py run <ts 0|1> [filter]   # 20 s per pose/res; ts = MC2_VK_GPU_TIMESTAMPS
       py -3 tools/perf/_census_v3.py walk                    # bench_freeze + bench_walk 200 (late commands)
       py -3 tools/perf/_census_v3.py battle                  # mc2_24 + 130 clones (about 200 movers), attack order
       py -3 tools/perf/_census_v3.py analyze                 # prints the tables
Pauses (never takes the mouse) while another mc2.exe, not ours, is running.
"""
import json, sys, time, threading, statistics
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/perf"), str(ROOT / "scripts")]
import psutil
import run_battle_bench as battle
from smoke_lib import host_lease, runner

EXE = Path("E:/mc2-installs/lane8/mc2.exe")
OUT = Path("E:/mc2-archive-2026-10/proofs/census3")
BK = ROOT / "tests/visual/bookmarks"
POSES = [("mc2_01", "cross_map_view", "mc2_01"), ("mc2_03", "scout_1", "mc2_03_camera"),
         ("mc2_03", "scout_2", "mc2_03_camera"), ("mc2_03", "scout_3", "mc2_03_camera"),
         ("mc2_03", "scout_4", "mc2_03_camera"), ("mc2_10", "wolfman", "mc2_10_camera"), ("mc2_10", "corner_far", "mc2_10_corner"),
         ("mc2_24", "wolfman", "mc2_24_camera"), ("mc2_24", "corner_far", "mc2_24_corner"), ("mc2_17", "corner_far", "mc2_17_camera")]
RES = {"1080": (1920, 1080), "2160": (3840, 2160)}


def pose(stem, name, f):
    return next(m for m in json.loads((BK / f"{f}.json").read_text())["bookmarks"] if m["name"] == name)


def wait_owner_quiet():
    while True:
        others = [p.pid for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == "mc2.exe"]
        if not others:
            return
        print("owner/other mc2.exe running", others, "- paused", flush=True)
        time.sleep(30)


def run(mission, name, d, cmds, seconds, w, h, ts, late_cmds=()):
    d.mkdir(parents=True, exist_ok=True)
    ch = d / "commands.txt"
    ch.write_text("\n".join([f"bench_output {d / 'frames.jsonl'}"] + cmds) + "\n", encoding="utf-8")
    env = battle.environment(ch, w, h)
    env["MC2_VK_GPU_TIMESTAMPS"] = str(ts)
    if late_cmds:
        def late():
            ack = ch.with_suffix(ch.suffix + ".ack")
            while not (ack.exists() and "bench_view" in ack.read_text(errors="replace")):
                time.sleep(0.2)
            time.sleep(4)
            ch.write_text("\n".join(late_cmds) + "\n", encoding="utf-8")
        threading.Thread(target=late, daemon=True).start()
    wait_owner_quiet()
    with host_lease.exclusive_window(f"census3 {mission} {name}", timeout_s=1500):
        res = runner.run_one(runner.RunConfig(exe=[str(EXE)], profile="stock", stem=mission, duration=seconds,
            heartbeat_timeout_load_s=180, heartbeat_timeout_play_s=15, grace_s=150, env_extra=env, show_window=True))
    (d / "engine.log").write_text(res.stdout_text, encoding="utf-8")
    ok = res.exit_code == 0 and not res.killed_by_timeout and res.verdict.passed
    print(f"{d.name} ok={ok}", flush=True)
    if not ok:
        print("  why: exit", res.exit_code, "timeout", res.killed_by_timeout, "verdict", str(res.verdict)[:600], flush=True)


def load(d):
    rows = [json.loads(l) for l in (d / "frames.jsonl").read_text().splitlines() if l.strip()]
    cpu, t, out = [r for r in rows if r.get("type") == "cpu" and "wait_sealed_ms" in r], 0.0, []
    for r in cpu:
        t += r["ms"]
        if t > 5000 and r["ms"] < 200:
            out.append(r)
    gpu = [r for r in rows if r.get("type") == "gpu" and r["frame"] > 100 and r["ms"] < 1000]
    return out, gpu, [r for r in rows if r.get("type") == "tick"]


def pct(v, q):
    v = sorted(v)
    return v[int((len(v) - 1) * q)] if v else 0.0


def avg(v):
    return sum(v) / len(v) if v else 0.0


def analyze():
    print("| pose | res | fps | cpu ms mean / p95 | main_exec | wait_sealed | wait_sim | GPU ms med / p95 | bound | draws props/terr/mech/shad/vfx/water | movers |")
    print("|---|---|---:|---|---:|---:|---:|---|---|---|---|")
    for mission, name, f in POSES:
        for rk in RES:
            a, b = OUT / f"{mission}_{name}_{rk}_ts0", OUT / f"{mission}_{name}_{rk}_ts1"
            if not (a / "frames.jsonl").exists():
                continue
            cpu, _, _ = load(a)
            ms = [r["ms"] for r in cpu]
            dr = {k: avg([r["draws"][k] for r in cpu]) for k in cpu[0]["draws"]}
            gm = gp = None
            if (b / "frames.jsonl").exists():
                _, g, _ = load(b)
                gm, gp = statistics.median([r["ms"] for r in g]), pct([r["ms"] for r in g], .95)
            mean = avg(ms)
            bound = "n/a" if gm is None else ("GPU" if gm > 0.85 * mean else "CPU")
            sealed = avg([r["wait_sealed_ms"] for r in cpu]); sim = avg([r["wait_sim_ms"] for r in cpu])
            print(f"| {mission} {name} | {rk}p | {1000 / mean:.0f} | {mean:.2f} / {pct(ms, .95):.2f} | {mean - sealed - sim:.2f} | {sealed:.2f} | {sim:.2f} | "
                  f"{'-' if gm is None else f'{gm:.2f} / {gp:.2f}'} | {bound} | {dr['props']:.0f}/{dr['terrain']:.0f}/{dr['mech']:.0f}/{dr['shadow']:.0f}/{dr['vfx']:.0f}/{dr['water']:.0f} | |")
    for d in sorted(OUT.glob("battle*")):
        if (d / "frames.jsonl").exists():
            cpu, g, tk = load(d)
            ms = [r["ms"] for r in cpu]
            w = [json.loads(l) for l in (d / "frames.jsonl").read_text().splitlines() if '"world"' in l]
            print(d.name, f"cpu mean {avg(ms):.2f} p95 {pct(ms, .95):.2f} fps {1000 / avg(ms):.0f} tick p50/p95/max",
                  f"{pct([t['ms'] for t in tk], .5):.2f}/{pct([t['ms'] for t in tk], .95):.2f}/{max([t['ms'] for t in tk] or [0]):.2f}",
                  "movers/active", w[-1]["movers"], w[-1]["active"])


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "run":
        ts = int(sys.argv[2]); flt = sys.argv[3] if len(sys.argv) > 3 else ""
        for rk, (w, h) in RES.items():
            for mission, name, f in POSES:
                if flt in f"{mission}_{name}":
                    run(mission, name, OUT / f"{mission}_{name}_{rk}_ts{ts}", [battle.view_command(pose(mission, name, f))], 20, w, h, ts)
    elif mode == "walk":
        for mission, name, f in [p for p in POSES if p[1] in ("wolfman", "cross_map_view")]:
            run(mission, name, OUT / f"walk_{mission}_{name}", [battle.view_command(pose(mission, name, f))], 16, 1920, 1080, 0,
                late_cmds=("bench_freeze on", "bench_walk 200"))
    elif mode == "battle":
        sc = json.loads((BK / "battle_bench_v1.json").read_text())
        extra = 130
        spawn = [f"spawn clone {-980 + i % 13 * 65} {2350 + i // 13 * 65} 0 0 pmp_dagger" for i in range(extra)]
        for tag, view in (("tactical", sc["bookmarks"][0]), ("wolfman", pose("mc2_24", "wolfman", "mc2_24_camera"))):
            run("mc2_24", tag, OUT / f"battle200_{tag}", spawn + ["select all", "order powerup", "bench_attack", battle.view_command(view), "list all"], 25, 1920, 1080, 0)
    elif mode == "analyze":
        analyze()

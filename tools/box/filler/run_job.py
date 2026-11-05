#!/usr/bin/env python3
"""Box idle filler: one perf job on GPU <gpu> (caller holds /workspace/locks/gpu<gpu>). usage: run_job.py <gpu>
Picks the pose with the fewest runs for the current nifty sha, runs the game 20 s (GPU timestamps off), writes
/workspace/filler/results/<sha>/<pose>-<gpu>-<n>.json (frame/MAIN_BUSY/wait_sim/tick stats + the last [PERF_COUNTERS v1] line)."""
import fcntl, json, os, shutil, subprocess, sys, time
from pathlib import Path

F = Path("/workspace/filler")
# key -> (mission, bookmark file, bookmark name); the survey poses (A:/Games/mc2-ops/studies/perf-survey-2026-11-04.md)
POSES = {
    "mc2_01-cross_map_view": ("mc2_01", "mc2_01", "cross_map_view"),
    "mc2_03-scout_1": ("mc2_03", "mc2_03_camera", "scout_1"),
    "mc2_10-wolfman": ("mc2_10", "mc2_10_camera", "wolfman"),
    "mc2_24-wolfman": ("mc2_24", "mc2_24_camera", "wolfman"),
    "mc2_10-oblique_default": ("mc2_10", "mc2_10_camera", "oblique_default"),
}
DURATION = 20
DIAG_TAGS = "GPU_CULL,LIGHTBAKE_PROOF,ANIM_GATE,TerrainLOD_prod,CONFIG,BUILD,DEVICE,SHADER_COMPILE,PERF_COUNTERS"


def pct(v, q):
    if not v:
        return 0.0
    s = sorted(v)
    pos = (len(s) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def stats(v):
    return {"median": round(pct(v, .5), 4), "p95": round(pct(v, .95), 4), "p99": round(pct(v, .99), 4), "max": round(max(v), 4) if v else 0.0}


def pick(sha, gpu):
    out = F / "results" / sha
    out.mkdir(parents=True, exist_ok=True)
    with open(out / ".pick.lock", "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        counts = {p: len(list(out.glob(f"{p}-*-*.json"))) + len(list(out.glob(f"{p}-*-*.run"))) for p in POSES}
        # a pose that failed twice at this sha is skipped (the stale-sha mc2_01 rc=3 was re-picked 40 times, about 1 GPU-hour)
        fails = {p: len(list((out / "failed").glob(f"{p}-*-*.log"))) for p in POSES}
        ok = [p for p in POSES if fails[p] < 2]
        if not ok:
            return None
        pose = min(ok, key=lambda p: (counts[p], list(POSES).index(p)))
        n = counts[pose] + 1
        (out / f"{pose}-{gpu}-{n}.run").write_text(str(os.getpid()))
    return out, pose, n


def main():
    gpu = int(sys.argv[1])
    sha = (F / "current_sha").read_text().strip()
    if gpu == 0:   # GPU 0 is present-bound (68-72 ms/frame): its perf numbers are not comparable, so perf poses run on GPUs 1-2 only
        sys.exit(2)
    picked = pick(sha, gpu)
    if picked is None:   # every pose already failed twice at this sha: wait for the next sync
        sys.exit(3)
    out, pose, n = picked
    run_marker = out / f"{pose}-{gpu}-{n}.run"
    mission, bkf, bkn = POSES[pose]
    bm = next(m for m in json.loads((F / "src/tests/visual/bookmarks" / f"{bkf}.json").read_text(encoding="utf-8"))["bookmarks"] if m["name"] == bkn)
    d = F / "tmp" / f"gpu{gpu}"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    (d / "commands.txt").write_text(f"bench_output {d / 'frames.jsonl'}\nbench_view {bm['pos'][0]} {bm['pos'][1]} {bm['rot']} {bm['alt']} {bm['proj']}\n", encoding="utf-8")
    env = dict(os.environ, MC2_TEST_SPAWN="1", MC2_UI_COMMAND_FILE=str(d / "commands.txt"), MC2_DETERMINISTIC_RNG="1", MC2_SMOKE_SEED="0xC0FFEE",
               MC2_RNG_SEED="0xC0FFEE", MC2_VK_GPU_TIMESTAMPS="0", MC2_FPS_CAP="0", MC2_VSYNC="0", MC2_WIDTH="1920", MC2_HEIGHT="1080",
               MC2_LAUNCHER_ENV_JSON="0", MC2_FORCE_ALL_VISIBLE="0", MC2_SMOKE_MODE="1", MC2_LOG="1", MC2_DIAG_TAGS=DIAG_TAGS, FILLER_GPU=str(gpu))
    # env.sh pins DRI_PRIME + cores; display 290+N (lanes use :90+N and :190+N)
    script = (f"source /workspace/gamerun/env.sh $FILLER_GPU; export TMPDIR={d}; cd {F}/install; "
              f"exec timeout 300 taskset -c $GR_CPUS xvfb-run -n $((290+FILLER_GPU)) -s '-screen 0 1920x1080x24' "
              f"./mc2 --profile smoke --mission {mission} --duration {DURATION}")
    load0 = os.getloadavg()[0]
    t0 = time.time()
    with open(d / "engine.log", "wb") as lg:
        rc = subprocess.run(["bash", "-c", script], env=env, stdout=lg, stderr=subprocess.STDOUT).returncode
    wall = time.time() - t0
    overlap = subprocess.run(["flock", "-n", str(F / "sync.lock"), "true"]).returncode != 0   # a sync/build was running during the job
    rows = []
    fj = d / "frames.jsonl"
    if fj.exists():
        rows = [json.loads(l) for l in fj.read_text(encoding="utf-8", errors="replace").splitlines() if l.strip()]
    cpu = [r for r in rows if r.get("type") == "cpu" and "wait_sealed_ms" in r]
    ticks = [r["ms"] for r in rows if r.get("type") == "tick"]
    cpu = cpu[len(cpu) // 4:]   # warmup quarter dropped (as the survey)
    if rc != 0 or len(cpu) < 100:
        fail = out / "failed"
        fail.mkdir(exist_ok=True)
        shutil.copy(d / "engine.log", fail / f"{pose}-{gpu}-{n}.log")
        run_marker.unlink(missing_ok=True)
        print(f"[filler] FAIL {sha[:9]} {pose} gpu{gpu} rc={rc} cpu_rows={len(cpu)}", flush=True)
        return 1
    perf = [l for l in (d / "engine.log").read_text(errors="replace").splitlines() if "[PERF_COUNTERS v1]" in l]
    ms = [r["ms"] for r in cpu]
    res = {
        "sha": sha, "sha_time": int(subprocess.run(["git", "-C", str(F / "src"), "log", "-1", "--format=%ct", sha], capture_output=True, text=True).stdout.strip() or 0),
        "port": (F / "current_port").read_text().strip() if (F / "current_port").exists() else "none",
        "pose": pose, "mission": mission, "bookmark": bkn, "gpu": gpu, "gpu_model": "RTX 3060", "run": n, "res": "1920x1080", "gpu_timestamps": 0,
        "duration_s": DURATION, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t0)), "wall_s": round(wall, 1), "cores": env.get("GR_CPUS", f"{8*gpu}-{8*gpu+7}"),
        "frames": len(cpu), "frame_ms": stats(ms),
        "main_busy_ms": stats([r["ms"] - r.get("wait_gpu_main_ms", 0.0) for r in cpu]),
        "main_exec_ms": stats([r["ms"] - r["wait_sealed_ms"] - r.get("wait_sim_ms", 0.0) - r.get("wait_gpu_main_ms", 0.0) for r in cpu]),
        "wait_sim_ms": stats([r.get("wait_sim_ms", 0.0) for r in cpu]),
        "wait_sealed_ms": stats([r["wait_sealed_ms"] for r in cpu]),
        "wait_gpu_main_ms": stats([r.get("wait_gpu_main_ms", 0.0) for r in cpu]),
        "tick_ms": {**stats(ticks), "count": len(ticks)},
        "draws_mean": {k: round(sum(r["draws"][k] for r in cpu) / len(cpu), 1) for k in cpu[0].get("draws", {})},
        "perf_counters_last": perf[-1][-1500:] if perf else "",
        "loadavg_start": round(load0, 1), "loadavg_end": round(os.getloadavg()[0], 1), "build_overlap": overlap,
    }
    (out / f"{pose}-{gpu}-{n}.json.tmp").write_text(json.dumps(res, indent=1), encoding="utf-8")
    os.replace(out / f"{pose}-{gpu}-{n}.json.tmp", out / f"{pose}-{gpu}-{n}.json")
    run_marker.unlink(missing_ok=True)
    shutil.rmtree(d, ignore_errors=True)
    print(f"[filler] ok {sha[:9]} {pose} gpu{gpu} #{n} frame med/p95 {res['frame_ms']['median']}/{res['frame_ms']['p95']} ms", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

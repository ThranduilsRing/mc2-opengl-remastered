#!/usr/bin/env python3
"""Integrated mc2_24 battle evidence; game launches belong to the existing harnesses."""
from __future__ import annotations

import argparse
import atexit
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import sys
import threading
import time

import manifest_schema as ms
import stage_root as stages
import run_visual_capture as capture
from smoke_lib import deploy_lease, fingerprint, host_lease, render_options_pin, runner

ROOT = Path(__file__).resolve().parents[1]
SCENARIO = ROOT / "tests/visual/bookmarks/battle_bench_v1.json"
DEFAULT_EXE = Path("G:/mc2-installs/mc2-win64-mapgen/mc2.exe")
PASSES = {
    "terrain": ("VKIsl.Terrain",),
    "shadows": ("VKIsl.Shadow.",), "vegetation": ("VKIsl.Vfx.Vegetation",),
    "props": ("VKIsl.Props",), "mechs": ("VKIsl.Mech",), "water": ("VKIsl.Water",),
    "post": ("VKIsl.Post.", "VKIsl.Present.Composite"), "vfx": ("VKIsl.Vfx",),
}


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {"n": len(values), "mean_ms": statistics.mean(values), "min_ms": values[0], "median_ms": statistics.median(values), "max_ms": values[-1],
            **{f"p{p}_ms": values[math.ceil(p / 100 * len(values)) - 1] for p in (50, 95, 99)}}


def sha_file(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def content_identity(install):
    # Stock is pinned: inactive mod payloads do not participate in file resolution.
    files = set(install.glob("*.fst"))
    for directory in ("data", "assets", "shaders"):
        files.update(p for p in (install / directory).rglob("*") if p.is_file()
                     and not any(x.lower() in {"cache", ".cache", "debug_state"} for x in p.parts)
                     and p.suffix.lower() not in {".log", ".tmp"})
    rows = [{"path": p.relative_to(install).as_posix(), "bytes": p.stat().st_size, "sha256": sha_file(p)}
            for p in sorted(files)]
    shader_rows = [r for r in rows if r["path"].startswith("shaders/")]
    content_rows = [r for r in rows if not r["path"].startswith("shaders/")]
    digest = hashlib.sha256(json.dumps(content_rows, sort_keys=True).encode()).hexdigest()
    shader_digest = hashlib.sha256(json.dumps(shader_rows, sort_keys=True).encode()).hexdigest()
    mods = [{"path": p.relative_to(install).as_posix(), "sha256": sha_file(p)} for p in sorted((install / "mods").glob("*/mod.json"))]
    return {"sha256": digest, "files": content_rows, "shaders_sha256": shader_digest, "shader_files": shader_rows,
            "active_mod": None, "installed_mod_manifests": mods,
            "mod_policy": "stock; inactive mod bytes excluded"}


def view_command(mark):
    return "bench_view {} {} {} {} {}".format(*mark["pos"][:2], mark["rot"], mark["alt"], mark["proj"])


def prepare(directory, scenario, phase):
    directory.mkdir(parents=True, exist_ok=True)
    channel = directory / "commands.txt"
    spawn = scenario["spawn"]
    commands = [f"bench_output {directory / 'frames.jsonl'}"]
    if phase == "night": commands.append("set_light night")
    for i in range(spawn["count"]):
        x = spawn["origin"][0] + i % spawn["columns"] * spawn["spacing"]
        y = spawn["origin"][1] + i // spawn["columns"] * spawn["spacing"]
        commands.append(f"spawn clone {x} {y} 0 0 {spawn['pilot']}")
    commands += ["select all", "order powerup", "bench_attack", view_command(scenario["bookmarks"][0]), "list all"]
    channel.write_text("\n".join(commands) + "\n", encoding="utf-8")
    return channel


def environment(channel, width, height):
    declared = {"MC2_TEST_SPAWN": "1", "MC2_UI_COMMAND_FILE": str(channel),
                "MC2_DETERMINISTIC_RNG": "1", "MC2_SMOKE_SEED": "0xC0FFEE", "MC2_RNG_SEED": "0xC0FFEE",
                "MC2_VK_GPU_TIMESTAMPS": "1", "MC2_FPS_CAP": "0", "MC2_VSYNC": "0",
                "MC2_WIDTH": str(width), "MC2_HEIGHT": str(height), "MC2_LAUNCHER_ENV_JSON": "0",
                "MC2_FORCE_ALL_VISIBLE": "0"}
    if os.environ.get("MC2_SIM_OVERLAP") in ("0", "1"):   # G7 A/B: the bench passes this one gate through
        declared["MC2_SIM_OVERLAP"] = os.environ["MC2_SIM_OVERLAP"]
    if os.environ.get("MC2_FORCE_DYNAMIC_BUILDINGS") in ("0", "1"):
        declared["MC2_FORCE_DYNAMIC_BUILDINGS"] = os.environ["MC2_FORCE_DYNAMIC_BUILDINGS"]
    return declared


def drive_views(channel, marks, duration, done, errors):
    # Acknowledgement anchors the schedule after loading and spawning, independent of lease waits.
    ack = channel.with_suffix(channel.suffix + ".ack")
    while not done.wait(0.1):
        if ack.exists() and "bench_attack -> ok" in ack.read_text(encoding="utf-8", errors="replace"):
            break
    for mark in marks[1:]:
        if done.wait(duration / len(marks)):
            return
        try:
            if channel.stat().st_size:
                raise RuntimeError("command channel not consumed")
            channel.write_text(view_command(mark) + "\n", encoding="utf-8")
        except Exception as exc:
            errors.append(str(exc))
            return


def read_frames(directory):
    rows = []
    for line in (directory / "frames.jsonl").read_text(encoding="utf-8").splitlines():
        rows.append(json.loads(line))
    return rows


# These scopes are timed inside an already measured inclusive scope. Other
# dotted names (e.g. Terrain.CullCompute) are separate passes, not children.
NESTED_GPU_SCOPES = {"VKIsl.Vfx.Vegetation", "VKIsl.Props.Trees"}


def gpu_pass_sum(passes):
    return sum(value for name, value in passes.items() if name not in NESTED_GPU_SCOPES)


def machine_load():
    """Sample outside the benchmark lifetime, so every mc2 PID is another run."""
    try:
        import psutil
        cpu = psutil.cpu_percent(interval=0.5)
        engines = []
        for proc in psutil.process_iter(["pid", "name", "exe"]):
            if (proc.info.get("name") or "").lower() == "mc2.exe":
                engines.append(dict(proc.info))
        return {"cpu_percent": cpu, "other_mc2_processes": engines, "sampled_at": time.time()}
    except Exception as exc:  # advisory evidence; unavailable data stays explicitly unknown
        return {"cpu_percent": None, "other_mc2_processes": None, "error": str(exc)}


def summarize(directory, log, scenario):
    rows = read_frames(directory)
    worlds = [r for r in rows if r["type"] == "world"]
    if not worlds:
        raise ValueError(f"no world evidence: {directory}")
    start = next((r["frame"] for r in rows if r["type"] == "clock" and "frame" in r), worlds[0]["frame"])
    samples = [r for r in rows if r.get("frame", 0) >= start]
    gpu = [r for r in samples if r["type"] == "gpu"]
    if not gpu or any(r["recorded"] != r["valid"] for r in gpu):
        raise ValueError("missing or incomplete per-frame GPU timestamps")
    cpu = [r for r in samples if r["type"] == "cpu"]
    ticks = [r for r in samples if r["type"] == "tick"]
    memory = [r for r in rows if r["type"] == "memory"]
    scopes = sorted({k for r in gpu for k in r["passes"]})
    by_pass = {}
    for name, prefixes in PASSES.items():
        matched = [k for k in scopes if any(k == p or (p.endswith(".") and k.startswith(p)) for p in prefixes)]
        # No matching producer means unmeasured, never a fabricated zero.
        by_pass[name] = {"scopes": matched, "timing": distribution([sum(r["passes"].get(k, 0) for k in matched) for r in gpu]) if matched else None}
    times = dict((name, float(value)) for name, value in re.findall(r"\[TIMING v1\] event=(\w+) elapsed_ms=([\d.]+)", log))
    ack = (directory / "commands.txt.ack").read_text(encoding="utf-8", errors="replace")
    spawn_acks = re.findall(r"spawn clone .*? -> (\w+) \(([^\n]+)\)", ack)
    if len(spawn_acks) != scenario["spawn"]["count"] or any(status != "ok" or "WARNING" in detail for status, detail in spawn_acks):
        raise ValueError("spawn fixture failed health/count checks; inspect commands.txt.ack")
    destruction = max(r["destroyed"] for r in worlds) - worlds[0]["destroyed"]
    if destruction <= 0 or not any(r["type"] == "orders" and r["attacks"] > 0 for r in rows):
        raise ValueError("battle did not produce destruction and attack orders")
    return {"gpu_frame": distribution([r["ms"] for r in gpu]), "gpu_passes": by_pass,
            "gpu_pass_sum": distribution([gpu_pass_sum(r["passes"]) for r in gpu]),
            "gpu_scopes": {k: distribution([r["passes"].get(k, 0) for r in gpu]) for k in scopes},
            "cpu_frame": distribution([r["ms"] for r in cpu]), "sim_tick": distribution([r["ms"] for r in ticks]),
            "tick_spikes_over_1ms": [r for r in ticks if r["ms"] > 1],
            "catch_up_bursts": [r for r in cpu if r["ticks"] > 1],
            "max_ticks_per_frame": max((r["ticks"] for r in cpu), default=0),
            "tick_model": "fixed_30hz_sim_tick; sim_tick is one sim pass (R2), a render frame runs zero or more passes",
            "memory": {"vram_peak_mib": max((r["gpu"]["proc_dedicated_mb"] for r in memory), default=-1),
                       "vk_allocated_peak_mib": max((r["gpu"]["vk_allocated_mb"] for r in memory), default=-1),
                       "working_set_peak_bytes": max((r["working_set_peak_bytes"] for r in memory), default=0),
                       "sampling": "every 120 frames; process working set is OS lifetime peak"},
            "load_ms": times.get("mission_ready"), "world": {"movers_peak": max(r["movers"] for r in worlds),
                "units_alive_peak": max(r["units_alive"] for r in worlds), "mechs_peak": max(r["mechs"] for r in worlds),
                "new_destructions": destruction, "samples": worlds},
            "views": [r for r in rows if r["type"] == "view"], "warmup_sim_seconds": 0,
            "driver_receipts": [line for line in log.splitlines() if re.search(r"\[.*(?:DEVICE|VK_DEVICE|VULKAN).*\]|driver|deviceName", line, re.I)],
            "scope_note": "Pass sum excludes nested Vfx.Vegetation and Props.Trees. GPU frame is the timestamp span, not a sum of passes; gaps and overlap can differ.",
            "raw": str(directory / "frames.jsonl")}


def verify_runtime(log, expected_sha):
    observed = fingerprint.parse_fingerprint(log)
    if not observed or observed["dirty"] or not expected_sha.startswith(observed["sha"]):
        raise ValueError(f"runtime build mismatch: expected {expected_sha}, observed {observed}")


def perf_run(exe, directory, scenario, phase, size, duration, expected_sha):
    channel = prepare(directory, scenario, phase)
    declared = environment(channel, *size)
    env, record = capture.build_capture_env(SCENARIO, directory, 0, 30, False, declared)
    for key in ("MC2_VISUAL_BOOKMARK_CAPTURE", "MC2_VISUAL_CAPTURE_DIR", "MC2_VISUAL_CAPTURE_FRAME", "MC2_VISUAL_SETTLE"):
        env.pop(key, None)
        for section in record.values():
            if isinstance(section, dict): section.pop(key, None)
    actual = os.environ.copy()
    for key in runner.unregistered_ambient(actual, env): actual.pop(key, None)
    actual.update(runner.FORCED_ENV)
    for key, value in runner.SOFT_ENV.items(): actual.setdefault(key, value)
    for key in runner.POPPED_ENV: actual.pop(key, None)
    actual.update(env)
    record["effective"] = {k: v for k, v in actual.items() if k.startswith("MC2_")}
    load_before = machine_load()
    done, errors = threading.Event(), []
    thread = threading.Thread(target=drive_views, args=(channel, scenario["bookmarks"], duration, done, errors), daemon=True)
    thread.start()
    try:
        result = runner.run_one(runner.RunConfig(exe=[str(exe)], profile="stock", stem=scenario["mission"],
            duration=duration, heartbeat_timeout_load_s=180, heartbeat_timeout_play_s=15,
            grace_s=240, env_extra=env, show_window=True))
    finally:
        done.set(); thread.join(timeout=2)
    load_after = machine_load()
    (directory / "engine.log").write_text(result.stdout_text, encoding="utf-8")
    (directory / "environment.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    verify_runtime(result.stdout_text, expected_sha)
    if result.exit_code or result.killed_by_timeout or not result.verdict.passed or errors:
        raise RuntimeError(f"smoke failed: {directory}; exit={result.exit_code}; verdict={result.verdict}; camera={errors}")
    summary = summarize(directory, result.stdout_text, scenario)
    summary["machine_load"] = {"before": load_before, "after": load_after}
    if len(summary["views"]) != len(scenario["bookmarks"]):
        raise ValueError("camera sweep incomplete")
    return summary


def capture_run(exe, directory, scenario, phase, expected_sha):
    channel = prepare(directory, scenario, phase)
    declared = environment(channel, 1920, 1080)
    command = [str(ROOT / "scripts/run_visual_capture.py"), "--no-kill", "--show-window",
               "--exe", str(exe), "--mission", scenario["mission"], "--bookmarks", str(SCENARIO),
               "--out-dir", str(directory), "--trigger-frame", str(scenario["capture_frame"]),
               "--settle", str(scenario["settle_frames"]), "--duration", "120"]
    for key, value in declared.items():
        command += ["--env-declare", f"{key}={value}"]
    saved_argv = sys.argv
    try:
        sys.argv = command
        with (directory / "harness.log").open("w", encoding="utf-8") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            code = capture.main()
    finally:
        sys.argv = saved_argv
    if code:
        raise RuntimeError(f"capture refused/failed: {directory}; see harness.log")
    verify_runtime((directory / f"{scenario['mission']}_capture.log").read_text(encoding="utf-8", errors="replace"), expected_sha)
    return [str(directory / f"{scenario['mission']}_{m['name']}.png") for m in scenario["bookmarks"]]


def montage(paths, target):
    from PIL import Image, ImageDraw
    width, height = 960, 540
    canvas = Image.new("RGB", (width * 2, (height + 30) * math.ceil(len(paths) / 2)))
    draw = ImageDraw.Draw(canvas)
    for i, path in enumerate(paths):
        with Image.open(path) as source:
            canvas.paste(source.convert("RGB").resize((width, height)), (i % 2 * width, i // 2 * (height + 30) + 30))
        draw.text((i % 2 * width + 10, i // 2 * (height + 30) + 7), f"{Path(path).parent.name}: {Path(path).stem}", fill="white")
    canvas.save(target)


def table(report):
    lines = ["| Run / scene | GPU p50/p95/p99 ms | Pass sum p50/p95/p99 ms | Host CPU before/after % | Other mc2 PIDs before/after | CPU p95/p99 ms | Tick p95/p99 ms | VRAM MiB | Load ms |", "|---|---:|---:|---:|---|---:|---:|---:|---:|"]
    for run in report["runs"]:
        for key, row in run["perf"].items():
            def timing(name, ps):
                return "/".join(f"{row[name][f'p{p}_ms']:.3f}" for p in ps)
            load = row.get("machine_load", {})
            host_cpu = "/".join(str(load.get(phase, {}).get("cpu_percent", "unknown")) for phase in ("before", "after"))
            other_pids = "/".join(str([p["pid"] for p in load.get(phase, {}).get("other_mc2_processes") or []])
                                  if load.get(phase, {}).get("other_mc2_processes") is not None else "unknown"
                                  for phase in ("before", "after"))
            pass_sum = timing("gpu_pass_sum", (50,95,99)) if row.get("gpu_pass_sum") else "unmeasured"
            lines.append(f"| {run['index']} / {key} | {timing('gpu_frame', (50,95,99))} | {pass_sum} | {host_cpu} | {other_pids} | {timing('cpu_frame', (95,99))} | {timing('sim_tick', (95,99))} | {row['memory']['vram_peak_mib']} | {row['load_ms']} |")
    return lines


def write_report(report, out):
    lines = [f"# Integrated battle baseline {report['sha'][:12]}", "", *table(report), "", "GPU pass p50/p95/p99 (ms; absent scope = unmeasured):", "",
             "| Run / scene | Pass | p50 | p95 | p99 |", "|---|---|---:|---:|---:|"]
    notes = []
    for run in report["runs"]:
        for scene, row in run["perf"].items():
            for name, entry in row["gpu_passes"].items():
                t = entry["timing"]
                cells = " | ".join(f"{t[f'p{p}_ms']:.3f}" if t else "unmeasured" for p in (50,95,99))
                lines.append(f"| {run['index']} / {scene} | {name} | {cells} |")
            notes.append(f"{run['index']}/{scene}: {len(row['tick_spikes_over_1ms'])} ticks >1ms; {len(row['catch_up_bursts'])} catch-up bursts; max {row['max_ticks_per_frame']} ticks/frame; {row['world']['new_destructions']} destructions; {row['world']['movers_peak']} movers.")
    lines += ["", *notes, "", "Load policy: first launch vs repeated launch; OS/driver disk caches uncontrolled. CPU frame is elapsed work before the FPS cap, including driver waits. GPU frame is the first-to-last timestamped scope span, excluding present/vsync; pass sum excludes nested Vfx.Vegetation and Props.Trees. Host load is sampled before and after each process, not continuously; every mc2 PID in these snapshots belongs to another run. Remaining scopes are in JSON. VRAM is sampled, working set is OS lifetime peak.", "", "Variance (p95 max-min / mean):", "", "```json", json.dumps(report.get("variance", {}), indent=2), "```", "", "Budget overruns (ms above p95 ceiling):", "", "```json", json.dumps(report.get("budget_violations", []), indent=2), "```", "", f"Montage: {out / 'montage.png'}"]
    (out / "battle-bench.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (out / "battle-bench.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def diff(before, after):
    if before["scenario_sha256"] != after["scenario_sha256"] or before["content"]["sha256"] != after["content"]["sha256"]:
        raise ValueError("scenario/content mismatch: runs are not comparable")
    for key in ("profile", "render_options", "duration_s"):
        if before.get(key) != after.get(key):
            raise ValueError(f"{key} mismatch: runs are not comparable")
    print("| Scene / metric p95 ms | Before | After | Change |\n|---|---:|---:|---:|")
    for scene in before["runs"][0]["perf"]:
        for metric in ("gpu_frame", "cpu_frame", "sim_tick"):
            a = statistics.mean(r["perf"][scene][metric]["p95_ms"] for r in before["runs"])
            b = statistics.mean(r["perf"][scene][metric]["p95_ms"] for r in after["runs"])
            print(f"| {scene} / {metric} | {a:.3f} | {b:.3f} | {(b/a-1)*100:+.1f}% |")


def baseline_command(args, base_out):
    """Child invocation that benches the baseline exe with the same shape as the current run."""
    cmd = [sys.executable, str(Path(__file__).resolve()), "--exe", str(args.baseline_exe), "--out", str(base_out), "--allow-non-head",
           "--runs", str(args.runs), "--duration", str(args.duration), "--resolution", args.resolution]
    return cmd + (["--acceptance"] if args.acceptance else [])


def compare_with_baseline(args, out):
    import subprocess
    base_out = out.with_name(out.name + "-baseline")
    print(f"[battle-bench] BASELINE run: {args.baseline_exe} -> {base_out}", flush=True)
    rc = subprocess.call(baseline_command(args, base_out))  # background-launch: allow re-runs this same bench script (python child); it launches the game through its own offscreen path
    if rc:
        print(f"[battle-bench] baseline run failed rc={rc}", flush=True)
        return rc
    print("[battle-bench] BASELINE (before) vs CURRENT (after)", flush=True)
    diff(*(json.loads((d / "battle-bench.json").read_text(encoding="utf-8")) for d in (base_out, out)))
    return 0


def resolve_lengths(runs, duration, acceptance):
    """Iteration bench by default (1 run, 20 s); the 2 x 30 s acceptance shape only under --acceptance."""
    return runs or (2 if acceptance else 1), duration or (30 if acceptance else 20)


def hold_install(install):
    while True:
        try:
            deploy_lease.acquire_lease_waiting(str(install), "integrated battle benchmark", 30)
            break
        except deploy_lease.LeaseError:
            print("[battle-bench] install lease still occupied; retrying", flush=True)
    done = threading.Event()
    def refresh():
        while not done.wait(30): deploy_lease.touch_lease(str(install))
    thread = threading.Thread(target=refresh, daemon=True)
    thread.start()
    def release():
        done.set(); thread.join(timeout=2)
        deploy_lease.release_lease(str(install))
    atexit.register(release)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--runs", type=int, default=None, help="default 1 (2 with --acceptance)")
    parser.add_argument("--duration", type=int, default=None, help="default 20 (30 with --acceptance)")
    parser.add_argument("--acceptance", action="store_true",
                        help="orchestrator acceptance bench in a quiet window: hold the exclusive host lease around each launch "
                             "(without it launches use a normal slot and the numbers are provisional)")
    parser.add_argument("--resolution", choices=("all", "1080p", "4k"), default="all")
    parser.add_argument("--diff", nargs=2, type=Path, metavar=("BEFORE", "AFTER"))
    parser.add_argument("--baseline-exe", type=Path,
                        help="also bench this non-HEAD exe (a clean build of an earlier commit) with the same options and print "
                             "baseline vs --exe side by side; the baseline report lands in <out>-baseline")
    parser.add_argument("--allow-non-head", action="store_true", help=argparse.SUPPRESS)  # set for the baseline child run
    args = parser.parse_args()
    if args.diff:
        diff(*(json.loads(p.read_text(encoding="utf-8")) for p in args.diff)); return 0
    args.runs, args.duration = resolve_lengths(args.runs, args.duration, args.acceptance)
    if args.runs < 1 or not 20 <= args.duration <= 30:
        parser.error("--runs >=1 and --duration 20..30 required")
    started = time.monotonic()
    scenario = json.loads(SCENARIO.read_text(encoding="utf-8"))
    hold_install(args.exe.parent.resolve())
    identity = ms.identity_block(generator="battle_bench", exe_path=args.exe, repo_root=ROOT, deploy_target=args.exe.parent)
    provenance = identity["binary_provenance"]
    head_ok = args.allow_non_head or provenance.get("source_head_matches_binary")  # a baseline exe is clean but deliberately not HEAD
    if provenance.get("dirty") or not provenance.get("available") or not head_ok:
        raise ValueError("commit, build and deploy a clean binary first")
    sha = provenance["commit"] if args.allow_non_head else identity["git"]["commit"]
    out = (args.out or Path("E:/mc2-archive-2026-10/proofs/run_battle_bench") / sha).resolve()
    stages.preflight(out)
    out.mkdir(parents=True, exist_ok=True)
    if (out / "battle-bench.json").exists():
        raise ValueError("output already contains a run; choose --out to preserve the baseline")
    # Scrub ambient feature overrides; retain the host lease controls understood by the harness.
    for key in list(os.environ):
        if key.startswith("MC2_") and not key.startswith("MC2_HOST_LEASE"):
            del os.environ[key]
    report = {"schema": "battle-bench/1", "sha": sha, "identity": identity, "scenario": scenario, "build_config": "RelWithDebInfo",
              "scenario_sha256": sha_file(SCENARIO), "content": None, "profile": "stock/default-on native Vulkan",
              "render_options": render_options_pin.record(args.exe.parent, {})["values"], "duration_s": args.duration,
              "acceptance": args.acceptance,
              "load_policy": "first-process (cold) and repeat-process (warm); OS cache not evicted", "runs": []}
    held_s = [0.0]
    def measured(run):
        """Exclusive host lease for one launch only; hashing, summaries and waits between launches use no slot."""
        while True:
            try:
                with (host_lease.exclusive_window("integrated battle benchmark") if args.acceptance else host_lease.shared_window()):
                    if sha_file(args.exe) != identity["exe"]["sha256"]:
                        raise ValueError("install binary changed while waiting; redeploy the recorded build")
                    t0 = time.monotonic()
                    try:
                        return run()
                    finally:
                        held_s[0] += time.monotonic() - t0
            except host_lease.HostLeaseBusy:
                print("[battle-bench] lease still occupied; retrying", flush=True)
                time.sleep(1)
    images = []
    for i in range(1, args.runs + 1):
        run = {"index": i, "perf": {}, "captures": []}
        for phase in ("day", "night"):
            sizes = (("4k", (3840, 2160)), ("1080p", (1920, 1080)))
            for name, size in sizes:
                if args.resolution != "all" and name != args.resolution:
                    continue
                key = f"{phase}-{name}"
                print(f"[battle-bench] run {i}/{args.runs} {key}", flush=True)
                run["perf"][key] = measured(lambda: perf_run(args.exe.resolve(), out / f"run{i}" / key, scenario, phase, size, args.duration, sha))
            paths = measured(lambda: capture_run(args.exe.resolve(), out / f"run{i}" / f"capture-{phase}", scenario, phase, sha))
            run["captures"] += paths
            if i == args.runs:
                images += paths
        report["runs"].append(run)
        write_report(report, out)
    report["variance"] = {scene: {metric: (lambda v: (max(v)-min(v))/statistics.mean(v)*100)(
        [r["perf"][scene][metric]["p95_ms"] for r in report["runs"]]) for metric in ("gpu_frame", "cpu_frame", "sim_tick")}
        for scene in report["runs"][0]["perf"]}
    violations = []
    for scene in report["runs"][0]["perf"]:
        four_k = scene.endswith("4k")
        budgets = [("gpu_frame", 5.0)] if four_k else [("cpu_frame", 1.0)]
        budgets.append(("terrain", 0.5 if four_k else 0.15))
        for metric, budget in budgets:
            values = [(r["perf"][scene]["gpu_passes"]["terrain"]["timing"] if metric == "terrain" else r["perf"][scene][metric]) for r in report["runs"]]
            if any(v is None for v in values):
                continue
            measured = statistics.mean(v["p95_ms"] for v in values)
            if measured > budget:
                violations.append({"scene": scene, "metric": metric, "p95_ms": measured, "budget_ms": budget, "over_ms": measured-budget})
    report["budget_violations"] = sorted(violations, key=lambda v: v["over_ms"], reverse=True)
    print("[battle-bench] hashing runtime content after timing (preserves the first-load cache state)", flush=True)
    report["content"] = content_identity(args.exe.parent)
    montage(images, out / "montage.png")
    write_report(report, out)
    print("\n".join(table(report)))
    print(f"[battle-bench] {'exclusive' if args.acceptance else 'shared-slot (provisional)'} launches took {held_s[0]:.0f} s of {time.monotonic() - started:.0f} s wall")
    print(f"[battle-bench] evidence: {out}")
    if args.baseline_exe:
        return compare_with_baseline(args, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

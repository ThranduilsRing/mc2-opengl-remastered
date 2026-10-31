#!/usr/bin/env python3
"""Profile mission CPU zones with Tracy; default: four missions and battle bench.

Example (quiet host): py -3 tools/perf/mission_cpu_profile.py --install <lane-install>
Smoke: add --missions mc2_01 --seconds 10. No builds or deploys are performed.
Requires the existing Tracy tools and psutil. All timings are elapsed CPU-zone
time, including waits, not scheduled CPU consumption. Nested zones overlap.
"""
import argparse
from collections import defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from statistics import median
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "scripts")]
import tracy_capture_compare as tracy
import run_battle_bench as battle
from smoke_lib import child_registry, host_lease, nofocus, runner

DEFAULT_MISSIONS = ["mc2_01", "mc2_03", "mc2_10", "mc2_24", "battle"]
OUT = Path("E:/mc2-archive-2026-10/proofs/cpu-profile")
# Sibling scopes in gameosmain.cpp / gameosmain_frame.cpp. GameLogic includes
# 0..N fixed simulation ticks and dispatch/wait when sim overlap is enabled.
SPLIT = {"sim tick/logic": "GameLogic", "render": "DrawScreen", "present": "SwapWindow"}
CAMERA_MISSIONS = {"mc2_01", "mc2_10", "mc2_24"}
BOOKMARK_DIR = ROOT / "tests/visual/bookmarks"


def camera_bookmarks(mission):
    return json.loads((BOOKMARK_DIR / f"{mission}_camera.json").read_text(encoding="utf-8"))


def distribution(values):
    return ({"mean_ms": sum(values) / len(values), "p95_ms": p95(values),
             "worst_ms": max(values), "count": len(values)} if values else "unmeasured")


def drive_bookmarks(channel, marks, captures, mission, done, errors):
    """One launch: frozen PNG sweep, then live timed views; acknowledgements anchor timing."""
    try:
        deadline = time.monotonic() + 1440  # bounded normal lease wait plus mission load
        while not done.wait(.1):
            if all((captures / f"{mission}_{m['name']}{suffix}").is_file()
                   for m in marks for suffix in (".png", ".json")):
                break
            if time.monotonic() > deadline:
                raise RuntimeError("bookmark PNG sweep did not finish")
        ack = channel.with_suffix(".txt.ack")
        def send(mark):
            command = battle.view_command(mark)
            before = ack.stat().st_size if ack.exists() else 0
            channel.write_text(command + "\n", encoding="utf-8")
            until = time.monotonic() + 10
            while not done.wait(.01):
                receipt = ack.read_text(encoding="utf-8")[before:] if ack.exists() else ""
                if f"{command} -> ok" in receipt:
                    return
                if time.monotonic() > until:
                    raise RuntimeError(f"camera command unacknowledged: {command}")
            raise RuntimeError("game ended during camera schedule")
        # Allow the last readback to drain and the sweep to restore pause state.
        if done.wait(.5):
            return
        for mark in marks:
            send(mark)
            if mark["name"] == "orbit":
                start = time.monotonic()
                for step in range(1, 101):
                    if done.wait(max(0, start + step / 10 - time.monotonic())):
                        raise RuntimeError("orbit interrupted")
                    send(dict(mark, rot=mark["rot"] + step * 3.6))
                if time.monotonic() - start > 10.5:
                    raise RuntimeError("orbit command channel could not sustain 10 s sweep")
            elif done.wait(1.5):
                raise RuntimeError("bookmark schedule interrupted")
        # Sentinel closes the final measurement interval.
        send(marks[0])
    except Exception as exc:
        errors.append(str(exc))


def bookmark_measurements(directory, marks, log):
    rows = [json.loads(line) for line in (directory / "frames.jsonl").read_text().splitlines()]
    views = [r for r in rows if r["type"] == "view"]
    expected = len(marks) + 101  # 100 orbit steps and closing sentinel
    if len(views) != expected:
        raise RuntimeError(f"camera schedule incomplete: {len(views)}/{expected} views")
    result, cursor = {}, 0
    gpu_available = "[VK_GPU_PASSES v1]" in log
    for mark in marks:
        steps = 101 if mark["name"] == "orbit" else 1
        first, end = views[cursor]["frame"], views[cursor + steps]["frame"]
        cursor += steps
        # Exclude teleport and pipeline latency; retain all frames of the moving orbit.
        start = first + (1 if mark["name"] == "orbit" else 4)
        samples = [r for r in rows if start <= r.get("frame", -1) < end]
        gpu = [r for r in samples if r["type"] == "gpu" and r["recorded"] == r["valid"]]
        cpu = [r["ms"] for r in samples if r["type"] == "cpu"]
        scopes = sorted({k for r in gpu for k in r["passes"]}) if gpu_available else []
        png = directory / f"{directory.name}_{mark['name']}.png"
        captured = json.loads(png.with_suffix(".json").read_text(encoding="utf-8"))
        actual = {key: captured.get(key) for key in ("cap_pos", "cap_rot", "cap_proj", "cap_alt")}
        delta = abs(((actual["cap_rot"] - mark["rot"] + 180) % 360) - 180)
        pose_matches = (all(abs(a - b) <= 2 for a, b in zip(actual["cap_pos"][:2], mark["pos"][:2]))
                        and abs(actual["cap_proj"] - mark["proj"]) <= .5 and delta <= .5)
        result[mark["name"]] = {"pose": mark, "frame_range": [start, end],
            "captured_pose": actual, "pose_matches": pose_matches, "visual_review": "unreviewed",
            "cpu_frame": distribution(cpu), "zones": {},
            "gpu_span": distribution([r["ms"] for r in gpu]) if gpu_available else "unmeasured",
            "gpu_passes": {k: distribution([r["passes"].get(k, 0) for r in gpu]) for k in scopes},
            "resolution": [1920, 1080], "last_cpu_frame": max(r["frame"] for r in rows if r["type"] == "cpu"),
            "capture": str(png)}
        if not cpu:
            raise RuntimeError(f"no CPU samples for {mark['name']}")
    return result


def p95(values):
    return sorted(values)[max(0, math.ceil(len(values) * .95) - 1)] if values else None


def summarize(reader, frame_range=None, bookmarks=None, warmup_seconds=0):
    """Aggregate real individual events, merging identical names across threads."""
    events = []
    for row in reader:
        column = next((key for key in ("ns", "duration_ns", "exec_time_ns", "duration")
                       if key in row), None)
        if column is None or not row.get("name"):
            raise ValueError("Tracy unwrap CSV lacks name or event duration")
        value = int(row[column])
        if value < 0:
            raise ValueError("negative Tracy zone duration")
        events.append((sys.intern(row["name"]), int(row["ns_since_start"]) if row.get("ns_since_start") else None, value))
    frames = sorted((start, duration) for name, start, duration in events if name == "Frame" and start is not None)
    if frames and warmup_seconds:
        cutoff = frames[0][0] + warmup_seconds * 1e9
        frames = [(s, d) for s, d in frames if s >= cutoff]
    def aggregate(window=None):
        selected = frames if window is None else frames[window[0] - 1:window[1] - 1]
        if window is not None and len(selected) != window[1] - window[0]:
            raise ValueError("bookmark frame range missing from Tracy")
        start_frame = selected[0][0] if selected else None
        end_frame = max(start + duration for start, duration in selected) if selected else None
        durations = defaultdict(list)
        for name, start, duration in events:
            if selected and (start is None or start < start_frame or start + duration > end_frame):
                continue
            durations[name].append(duration)
        if not durations:
            raise ValueError("Tracy export contains no CPU events")
        return {name: {"count": len(values), "total_ms": sum(values) / 1e6,
                       "p95_ms": p95(values) / 1e6,
                       **({"median_ms": median(values) / 1e6,
                           "min_ms": min(values) / 1e6,
                           "q25_ms": sorted(values)[(len(values) - 1) // 4] / 1e6}
                          if frames else {}),
                       "worst_ms": max(values) / 1e6} for name, values in durations.items()}
    if bookmarks:
        expected = next(iter(bookmarks.values()))["last_cpu_frame"]
        if len(frames) != expected:
            raise RuntimeError(f"Tracy/frame counter alignment missing: {len(frames)} Frame scopes, last CPU frame {expected}")
        for mark in bookmarks.values():
            mark["zones"] = aggregate(mark["frame_range"])
    return aggregate(frame_range)


def capture_mission(install, mission, seconds, trace, extra_env):
    """Record through mission exit: a connection-time cutoff can capture only loading."""
    env = tracy.game_env("vk", False, 0, None, extra_env)
    if os.environ.get("MC2_VK_DEVICE"):
        env.setdefault("MC2_VK_DEVICE", os.environ["MC2_VK_DEVICE"])
    env["MC2_FORCE_ALL_VISIBLE"] = "0"  # profile normal mission visibility, overriding smoke's forced value
    marks = (camera_bookmarks(mission)["bookmarks"]
             if mission in CAMERA_MISSIONS and "MC2_UI_COMMAND_FILE" not in env else [])
    done, errors = threading.Event(), []
    driver = None
    if marks:
        channel = trace.parent / "commands.txt"
        channel.write_text(f"bench_output {trace.parent / 'frames.jsonl'}\n", encoding="utf-8")
        env.update({"MC2_TEST_SPAWN": "1", "MC2_UI_COMMAND_FILE": str(channel),
                    "MC2_VISUAL_BOOKMARK_CAPTURE": str(BOOKMARK_DIR / f"{mission}_camera.json"),
                    "MC2_VISUAL_CAPTURE_DIR": str(trace.parent), "MC2_VISUAL_CAPTURE_FRAME": "120",
                    "MC2_VISUAL_SETTLE": "30", "MC2_VK_GPU_TIMESTAMPS": "1",
                    "MC2_WIDTH": "1920", "MC2_HEIGHT": "1080", "MC2_FPS_CAP": "0", "MC2_VSYNC": "0",
                    "MC2_DETERMINISTIC_RNG": "1"})
        # The PNG sweep freezes simulation; the subsequent performance schedule resumes live simulation.
        env.pop("MC2_SMOKE_FIXED_TIMESTEP", None)
        driver = threading.Thread(target=drive_bookmarks,
            args=(channel, marks, trace.parent, mission, done, errors))
    # game_env returns a full environment; don't declare harness-only ambient
    # MC2_HOST_* settings as engine overrides under runner's strict gate check.
    for key in runner.unregistered_ambient(env):
        env.pop(key, None)
    options = tracy.render_options_pin.record(install, env)
    tracy.render_options_pin.write(tracy.options_sidecar(trace), options)
    flags = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}
    with trace.with_suffix(".capture.log").open("w", encoding="utf-8") as log:
        # background-launch: allow Tracy console recorder, never an engine.
        recorder = subprocess.Popen([str(tracy.tracy_exe("tracy-capture.exe")),
                                     "-a", "127.0.0.1", "-p", "8086", "-f", "-o", str(trace)],
                                    stdout=log, stderr=subprocess.STDOUT,
                                    **nofocus.hidden_kwargs(**flags))
        try:
            time.sleep(1)  # recorder binds before the on-demand Tracy client boots
            if driver:
                driver.start()
            result = runner.run_one(runner.RunConfig(
                exe=[str(install / "mc2.exe")], profile="stock", stem=mission,
                duration=seconds, heartbeat_timeout_load_s=120,
                heartbeat_timeout_play_s=15, grace_s=150, env_extra=env, show_window=True))
            trace.with_suffix(".game.log").write_text(result.stdout_text, encoding="utf-8")
            done.set()
            if driver:
                driver.join(timeout=15)
            if errors:
                raise RuntimeError("; ".join(errors))
            if result.exit_code or result.killed_by_timeout:
                return result.exit_code or 1
            recorder.wait(timeout=60)
            if recorder.returncode or not trace.is_file() or trace.stat().st_size < tracy.MIN_TRACE_BYTES:
                raise RuntimeError(f"missing/trivial Tracy capture; see {log.name}")
            return result.exit_code or int(result.killed_by_timeout)
        finally:
            done.set()
            if driver and driver.is_alive():
                driver.join(timeout=15)
            tracy._stop_capture_child(recorder)


def export_zones(trace, bookmarks=None, warmup_seconds=0):
    # Stream all events once: computing p95 from aggregate mean/max is invalid.
    with trace.with_suffix(".export.log").open("w", encoding="utf-8") as log:
        flags = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}
        # background-launch: allow Tracy CSV console exporter, never an engine.
        child = subprocess.Popen([str(tracy.tracy_exe("tracy-csvexport.exe")), "-u", str(trace)],
                                 stdout=subprocess.PIPE, stderr=log, text=True,
                                 **nofocus.hidden_kwargs(**flags))
        timer = threading.Timer(180, child.kill)
        timer.start()
        try:
            with child.stdout:
                zones = summarize(csv.DictReader(child.stdout), bookmarks=bookmarks, warmup_seconds=warmup_seconds)
            if child.wait(timeout=10):
                raise RuntimeError(f"Tracy export failed; see {log.name}")
            return zones
        finally:
            timer.cancel()
            if child.poll() is None:
                child.kill()
            child.wait()


def game_processes(psutil, exclude=()):
    """MC2 engines plus common game images/install paths; retain PID evidence."""
    found = []
    for proc in psutil.process_iter(["pid", "name", "exe"]):
        info = proc.info
        if info["pid"] in exclude:
            continue
        name = (info["name"] or "").lower()
        exe = (info["exe"] or "").replace("\\", "/").lower()
        if (name in child_registry.ENGINE_IMAGES
                or name in {"minecraft.exe",
                     "javaw.exe", "cs2.exe", "dota2.exe", "fortniteclient-win64-shipping.exe"}
                or "-win64-shipping.exe" in name
                or "/steamapps/common/" in exe or "/epic games/" in exe):
            found.append(info)
    return found


def external_cpu(per_core, reserved, game_cpu, sampler_cpu=0, sampler_reserved=True):
    """Process percentages use one-core units; all excluded work must be pinned.

    Games must be inside reserved; sampler affinity must be wholly inside or outside.
    """
    return {"whole_machine_external_cpu_percent": max(0, (sum(per_core) - game_cpu - sampler_cpu) / len(per_core)),
            "external_cpu_percent": max(0, (sum(per_core[c] for c in reserved) - game_cpu - (sampler_cpu if sampler_reserved else 0)) / len(reserved))}


class HostSamples:
    """Sample while capture is alive, excluding only this tool's descendants."""
    def __init__(self, psutil):
        self.psutil = psutil
        self.stop = threading.Event()
        self.rows = []
        self.error = None
        self.thread = threading.Thread(target=self.run, name="host-cpu-sample")

    def run(self):
        psutil = self.psutil
        parent = psutil.Process()
        reserved = host_lease.game_cores()
        original = parent.cpu_affinity()
        tool_cores = [c for c in original if c not in reserved]
        if tool_cores:
            parent.cpu_affinity(tool_cores)
        sampler_affinity = set(parent.cpu_affinity())
        parent.cpu_percent()
        known = {}
        psutil.cpu_percent(percpu=True)
        try:
            while not self.stop.wait(.5):
                children = parent.children(recursive=True)
                game_cpu = 0.0
                games = []
                for child in children:
                    try:
                        if child.name().lower() == "mc2.exe":
                            if child.pid not in known:
                                child.cpu_percent()
                                known[child.pid] = child
                                continue  # prime the process counter before subtraction
                            proc = known[child.pid]
                            if not set(proc.cpu_affinity()) <= set(reserved):
                                raise RuntimeError(f"game {proc.pid} affinity outside reserved cores {reserved}")
                            game_cpu += proc.cpu_percent()
                            games.append({"pid": proc.pid, "priority": proc.nice(),
                                          "affinity": proc.cpu_affinity()})
                    except (psutil.NoSuchProcess, psutil.AccessDenied):
                        continue
                cpu = psutil.cpu_percent(percpu=True)
                sampler_cpu = parent.cpu_percent()
                actual_affinity = set(parent.cpu_affinity())
                if actual_affinity != sampler_affinity:
                    sampler_affinity = actual_affinity
                    continue  # the guard moved the sampler; attribution is ambiguous this interval
                if sampler_affinity & set(reserved) and not sampler_affinity <= set(reserved):
                    raise RuntimeError("sampler affinity straddles measured reservation")
                self.rows.append({"time": time.time(), "host_cpu_percent": sum(cpu) / len(cpu),
                                  "reserved_cores": reserved, "per_core_cpu_percent": cpu,
                                  "game_cpu_percent_one_core": game_cpu, "sampler_cpu_percent_one_core": sampler_cpu,
                                  "sampler_affinity": sorted(sampler_affinity),
                                  **external_cpu(cpu, reserved, game_cpu, sampler_cpu, sampler_affinity <= set(reserved)),
                                  "other_games": game_processes(psutil, {c.pid for c in children}),
                                  "own_games": games})
        except Exception as exc:
            self.error = str(exc)
        finally:
            parent.cpu_affinity(original)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.thread.join(timeout=10)
        if self.thread.is_alive():
            self.error = "host sampler did not stop"


def noise_report(samples):
    active = [row for row in samples.rows if row["own_games"]]
    reasons = []
    if samples.error or not active:
        reasons.append(samples.error or "no in-game CPU samples")
    if any(row["other_games"] for row in samples.rows):
        reasons.append("other game processes observed")
    if any(len(row["own_games"]) > 1 for row in samples.rows):
        reasons.append("multiple owned game processes observed")
    external = p95([row["external_cpu_percent"] for row in active])
    if external is not None and external >= 15:
        reasons.append("reserved-core external CPU p95 reaches 15%")
    return {"status": "NOISY" if reasons else "quiet", "reasons": reasons,
            "host_cpu_p95": p95([row["host_cpu_percent"] for row in active]),
            "whole_machine_external_cpu_p95": p95([row.get("whole_machine_external_cpu_percent", row["external_cpu_percent"]) for row in active]),
            "external_cpu_p95": external, "external_cpu_scope": "reserved_cores", "samples": samples.rows}


def table(name, zones, noise, top):
    lines = [f"## {name} ({noise['status']})", "",
             f"Host CPU p95: {noise['host_cpu_p95']}%; whole-machine external CPU p95: {noise.get('whole_machine_external_cpu_p95')}%; reserved-core external CPU p95: {noise['external_cpu_p95']}%.",
             "; ".join(noise["reasons"]), "",
             "| rank by total | zone | total ms | event p95 ms | calls | p95 rank |",
             "|---:|---|---:|---:|---:|---:|"]
    tail = sorted(zones, key=lambda n: zones[n]["p95_ms"], reverse=True)
    totals = sorted(zones, key=lambda n: zones[n]["total_ms"], reverse=True)
    # One table contains the union of both top-N sets, preserving total order.
    selected = set(totals[:top]) | set(tail[:top])
    for rank, zone in enumerate(totals, 1):
        if zone in selected:
            row = zones[zone]
            label = zone.replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {rank} | {label} | {row['total_ms']:.3f} | {row['p95_ms']:.3f} | {row['count']} | {tail.index(zone) + 1} |")
    lines += ["", "| split | elapsed ms | event p95 ms | share of three scopes |",
              "|---|---:|---:|---:|"]
    # Worker ticks overlap present in wall time. Add their elapsed scope work to
    # logic, never subtract it from present or claim the shares are wall time.
    split_zones = dict(zones)
    if "SimOverlap.Tick" in zones:
        logic = dict(zones["GameLogic"])
        logic["total_ms"] += zones["SimOverlap.Tick"]["total_ms"]
        split_zones["GameLogic"] = logic
    total = sum(split_zones.get(zone, {}).get("total_ms", 0) for zone in SPLIT.values())
    for label, zone in SPLIT.items():
        row = split_zones.get(zone)
        if row:
            lines.append(f"| {label} ({zone}) | {row['total_ms']:.3f} | {row['p95_ms']:.3f} | {row['total_ms'] / total:.1%} |")
        else:
            lines.append(f"| {label} ({zone}) | MISSING | MISSING | MISSING |")
    if "SimOverlap.Tick" in zones:
        tick = zones["SimOverlap.Tick"]
        lines += ["", f"Sim worker tick batches: {tick['total_ms']:.3f} ms total, "
                  f"{tick['p95_ms']:.3f} ms p95, {tick['count']} batches (one or more ticks each). Included in sim total; "
                  "sim row p95 remains GameLogic p95. Worker ticks overlap present."]
    return "\n".join(lines)


def profile(args, name, directory, psutil):
    directory.mkdir(parents=True)
    env = []
    mission = name
    if name == "battle":
        scenario = json.loads(battle.SCENARIO.read_text(encoding="utf-8"))
        scenario["spawn"]["count"] = 0
        mission = scenario["mission"]
        channel = battle.prepare(directory, scenario, "day")
        env = [f"{k}={v}" for k, v in battle.environment(channel, 1920, 1080).items()]
        env.append("MC2_MOVER_FIXTURE=200")
    short_view = getattr(args, "short_view", True)
    view = None
    if short_view and name in CAMERA_MISSIONS:
        chosen = {"mc2_01": "sun_facing_low", "mc2_10": "sun_facing_low", "mc2_24": "low_horizon_0"}[name]
        view = next(m for m in camera_bookmarks(name)["bookmarks"] if m["name"] == chosen)
        channel = directory / "commands.txt"
        channel.write_text(f"bench_output {directory / 'frames.jsonl'}\n{battle.view_command(view)}\n", encoding="utf-8")
        env = [f"{k}={v}" for k, v in battle.environment(channel, 1920, 1080).items()]
    started = time.monotonic()
    # Standard bounded perf lease prevents a second MC2 instance feeding Tracy.
    with host_lease.exclusive_window(f"mission CPU perf {name}", timeout_s=1200):
        waited = time.monotonic() - started
        others = game_processes(psutil)
        if others:
            raise RuntimeError(f"refusing profile with other games: {others}")
        trace = directory / f"{name}.tracy"
        start_run = time.monotonic()
        samples = HostSamples(psutil)
        try:
            with samples:
                rc = capture_mission(args.install, mission, args.seconds, trace, env)
        finally:
            noise = noise_report(samples)
            (directory / "host-load.json").write_text(json.dumps(noise, indent=2), encoding="utf-8")
        run_seconds = time.monotonic() - start_run
    if rc:
        raise RuntimeError(f"capture failed: {trace}")
    log = trace.with_suffix(".game.log").read_text(encoding="utf-8", errors="replace")
    if ("[SMOKE v1] event=summary result=pass" not in log
            or "event=summary result=fail" in log or "Crash Report" in log
            or f"event=mission_resolve stem={mission} " not in log):
        raise RuntimeError(f"missing smoke receipts or crash: {trace}")
    if name == "battle":
        ack = channel.with_suffix(channel.suffix + ".ack")
        if not ack.exists() or "bench_attack -> ok" not in ack.read_text(encoding="utf-8", errors="replace"):
            raise RuntimeError("battle spawn/attack was not acknowledged")
    if short_view and view:
        ack = channel.with_suffix(".txt.ack").read_text(encoding="utf-8")
        if f"{battle.view_command(view)} -> ok" not in ack:
            raise RuntimeError("worst-view command was not acknowledged")
    if name == "battle":
        worlds = [r for r in battle.read_frames(directory) if r["type"] == "world"]
        if not worlds or any(r.get("movers") != 200 for r in worlds):
            raise RuntimeError("battle world evidence does not establish 200 movers")
    bookmarks = bookmark_measurements(directory, camera_bookmarks(name)["bookmarks"], log) if name in CAMERA_MISSIONS and not short_view else {}
    zones = export_zones(trace, bookmarks, warmup_seconds=3 if short_view else 0)
    missing = set(SPLIT.values()) - zones.keys()
    if missing:
        raise RuntimeError(f"missing frame scopes {sorted(missing)}; trace is not a mission profile")
    return {"mission": mission, "noise": noise, "zones": zones, "bookmarks": bookmarks,
            "view": view, "duration_seconds": args.seconds,
            "capture_clock": "wall-clock; review PNGs only, not deterministic goldens" if bookmarks else None,
            "lease_wait_seconds": waited, "game_run_seconds": run_seconds,
            "trace": str(trace)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", required=True, type=Path)
    parser.add_argument("--missions", nargs="+", choices=DEFAULT_MISSIONS, default=DEFAULT_MISSIONS)
    parser.add_argument("--seconds", type=int, default=20, help="mission gameplay duration, excluding startup")
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--out", type=Path, default=OUT)
    args = parser.parse_args(argv)
    if args.seconds <= 0 or args.top <= 0:
        parser.error("--seconds and --top must be positive")
    if "MC2_SMOKE_FIXED_TIMESTEP" in os.environ:
        parser.error("unset MC2_SMOKE_FIXED_TIMESTEP: CPU profiling requires wall-clock simulation")
    import psutil
    if os.name == "nt":
        psutil.Process().nice(psutil.IDLE_PRIORITY_CLASS)
    args.install = args.install.resolve()
    exe = args.install / "mc2.exe"
    if not exe.is_file():
        parser.error(f"missing {exe}")
    tracy.tracy_exe("tracy-capture.exe")
    tracy.tracy_exe("tracy-csvexport.exe")
    directory = args.out.resolve() / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory.mkdir(parents=True)
    report = {"exe": str(exe), "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "seconds": args.seconds, "runs": {}, "errors": {}}
    intro = ("# Mission CPU profile\n\n"
             "Elapsed inclusive CPU-zone time (includes waits), not OS CPU consumption. "
             "Tables contain the union of top-N by total and event p95. Nested zone totals overlap. "
             "The split uses GameLogic (+ SimOverlap.Tick worker time) / DrawScreen / SwapWindow; "
             "shares are elapsed scope work, not wall-time fractions, and exclude event handling, "
             "snapshot extraction and frame caps. GameLogic contains 0..N sim ticks, presentation "
             "and overlap dispatch/wait; worker ticks overlap present and are also reported separately. "
             "Trace records startup plus the mission; tables include only complete events inside "
             "the Frame span, excluding startup. NOISY results are provisional. Other-game detection "
             "covers MC2, common images, Steam/Epic paths; unknown executables may escape detection.\n")
    text = [intro]
    for name in dict.fromkeys(args.missions):
        try:
            run = profile(args, name, directory / name, psutil)
            report["runs"][name] = run
            rendered = table(name, run["zones"], run["noise"], args.top)
            for bookmark, data in run.get("bookmarks", {}).items():
                rendered += "\n\n" + table(f"{name}/{bookmark}", data["zones"], run["noise"], args.top)
                rendered += f"\n\nCPU frame: {data['cpu_frame']}; GPU span: {data['gpu_span']}."
            text.append(rendered)
            print(rendered, flush=True)
        except (Exception, SystemExit) as exc:
            report["errors"][name] = str(exc)
            noise_path = directory / name / "host-load.json"
            noise = (json.loads(noise_path.read_text(encoding="utf-8")) if noise_path.is_file() else
                     {"status": "NOISY", "reasons": [str(exc)], "host_cpu_p95": None,
                      "whole_machine_external_cpu_p95": None, "external_cpu_p95": None,
                      "external_cpu_scope": "reserved_cores", "samples": []})
            report["runs"][name] = {"noise": noise, "zones": {}, "bookmarks": {}, "error": str(exc),
                "trace": str(directory / name / f"{name}.tracy")}
            text.append(f"## {name}: FAILED\n\n{exc}")
            print(f"{name}: FAILED: {exc}", file=sys.stderr, flush=True)
        finally:
            (directory / "profile.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            (directory / "profile.md").write_text("\n\n".join(text) + "\n", encoding="utf-8")
    print(f"Proof: {directory}")
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

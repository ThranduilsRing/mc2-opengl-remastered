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


def p95(values):
    return sorted(values)[max(0, math.ceil(len(values) * .95) - 1)] if values else None


def summarize(reader):
    """Aggregate real individual events, merging identical names across threads."""
    durations = defaultdict(list)
    events = []
    for row in reader:
        column = next((key for key in ("ns", "duration_ns", "exec_time_ns", "duration")
                       if key in row), None)
        if column is None or not row.get("name"):
            raise ValueError("Tracy unwrap CSV lacks name or event duration")
        value = int(row[column])
        if value < 0:
            raise ValueError("negative Tracy zone duration")
        events.append((row["name"], int(row["ns_since_start"]) if row.get("ns_since_start") else None, value))
    frames = [(start, duration) for name, start, duration in events if name == "Frame" and start is not None]
    start_frame = min(start for start, _ in frames) if frames else None
    end_frame = max(start + duration for start, duration in frames) if frames else None
    for name, start, duration in events:
        if frames and (start is None or start < start_frame or start + duration > end_frame):
            continue
        durations[name].append(duration)
    if not durations:
        raise ValueError("Tracy export contains no CPU events")
    return {name: {"count": len(values), "total_ms": sum(values) / 1e6,
                   "p95_ms": p95(values) / 1e6} for name, values in durations.items()}


def capture_mission(install, mission, seconds, trace, extra_env):
    """Record through mission exit: a connection-time cutoff can capture only loading."""
    env = tracy.game_env("vk", False, 0, None, extra_env)
    env["MC2_FORCE_ALL_VISIBLE"] = "0"  # profile normal mission visibility, overriding smoke's forced value
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
            result = runner.run_one(runner.RunConfig(
                exe=[str(install / "mc2.exe")], profile="stock", stem=mission,
                duration=seconds, heartbeat_timeout_load_s=120,
                heartbeat_timeout_play_s=15, grace_s=150, env_extra=env, show_window=True))
            trace.with_suffix(".game.log").write_text(result.stdout_text, encoding="utf-8")
            if result.exit_code or result.killed_by_timeout:
                return result.exit_code or 1
            recorder.wait(timeout=60)
            if recorder.returncode or not trace.is_file() or trace.stat().st_size < tracy.MIN_TRACE_BYTES:
                raise RuntimeError(f"missing/trivial Tracy capture; see {log.name}")
            return result.exit_code or int(result.killed_by_timeout)
        finally:
            tracy._stop_capture_child(recorder)


def export_zones(trace):
    # Stream all events once: computing p95 from aggregate mean/max is invalid.
    with trace.with_suffix(".export.log").open("w", encoding="utf-8") as log:
        flags = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}
        child = subprocess.Popen([str(tracy.tracy_exe("tracy-csvexport.exe")), "-u", str(trace)],
                                 stdout=subprocess.PIPE, stderr=log, text=True,
                                 **nofocus.hidden_kwargs(**flags))
        timer = threading.Timer(180, child.kill)
        timer.start()
        try:
            with child.stdout:
                zones = summarize(csv.DictReader(child.stdout))
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
        known = {}
        psutil.cpu_percent()
        try:
            while not self.stop.wait(.5):
                children = parent.children(recursive=True)
                game_cpu = 0.0
                games = []
                for child in children:
                    try:
                        if child.name().lower() == "mc2.exe":
                            proc = known.setdefault(child.pid, child)
                            game_cpu += proc.cpu_percent()
                            games.append({"pid": proc.pid, "priority": proc.nice(),
                                          "affinity": proc.cpu_affinity()})
                    except (psutil.NoSuchProcess, psutil.AccessDenied):
                        continue
                cpu = psutil.cpu_percent()
                self.rows.append({"time": time.time(), "host_cpu_percent": cpu,
                                  "external_cpu_percent": max(0, cpu - game_cpu / (psutil.cpu_count() or 1)),
                                  "other_games": game_processes(psutil, {c.pid for c in children}),
                                  "own_games": games})
        except Exception as exc:
            self.error = str(exc)

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
    external = p95([row["external_cpu_percent"] for row in active])
    if external is not None and external > 25:
        reasons.append("external host CPU p95 exceeds 25%")
    return {"status": "NOISY" if reasons else "quiet", "reasons": reasons,
            "host_cpu_p95": p95([row["host_cpu_percent"] for row in active]),
            "external_cpu_p95": external, "samples": samples.rows}


def table(name, zones, noise, top):
    lines = [f"## {name} ({noise['status']})", "",
             f"Host CPU p95: {noise['host_cpu_p95']}%; external CPU p95: {noise['external_cpu_p95']}%.",
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
        mission = scenario["mission"]
        channel = battle.prepare(directory, scenario, "day")
        env = [f"{k}={v}" for k, v in battle.environment(channel, 1920, 1080).items()]
    started = time.monotonic()
    # A bounded exclusive window prevents a second MC2 instance feeding Tracy.
    with host_lease.exclusive_window(f"mission CPU perf {name}", timeout_s=120):
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
    zones = export_zones(trace)
    missing = set(SPLIT.values()) - zones.keys()
    if missing:
        raise RuntimeError(f"missing frame scopes {sorted(missing)}; trace is not a mission profile")
    return {"mission": mission, "noise": noise, "zones": zones,
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
        psutil.Process().nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
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
            text.append(rendered)
            print(rendered, flush=True)
        except (Exception, SystemExit) as exc:
            report["errors"][name] = str(exc)
            text.append(f"## {name}: FAILED\n\n{exc}")
            print(f"{name}: FAILED: {exc}", file=sys.stderr, flush=True)
        finally:
            (directory / "profile.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            (directory / "profile.md").write_text("\n\n".join(text) + "\n", encoding="utf-8")
    print(f"Proof: {directory}")
    return 1 if report["errors"] else (2 if any(r["noise"]["status"] == "NOISY" for r in report["runs"].values()) else 0)


if __name__ == "__main__":
    raise SystemExit(main())

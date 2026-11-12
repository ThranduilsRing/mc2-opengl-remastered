#!/usr/bin/env python3
"""Orchestrator-only historical tag evidence. Never builds, deploys or promotes.

Requires install-asset-parity-1. The laptop must receive an identified copy of
the v0.4 asset snapshot and its --reference-sha256 from the PC dry run.
Failure leaves diagnostics in fresh scratch; only complete bundles are published.
"""
from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/perf"))
import console_child
import run_battle_bench as bench
import sim_determinism as sim
from smoke_lib import deploy_lease, host_lease, vk_profile

MISSIONS = bench.STOCK_MISSIONS
DEFAULT_BOOKMARKS = Path("A:/Games/mc2-ops/bookmarks/mc2_01_owner_2026-11-11.json")  # drive-path: ok brief's canonical owner file; --bookmarks supplies the laptop copy
DEFAULT_OUT = Path("E:/mc2-archive-2026-10/proofs/by-tag")  # drive-path: ok brief's immutable archive; --out-root supplies laptop-local storage
V04 = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")  # drive-path: ok identifies the canonical PC snapshot; copied --asset-reference requires its hash
SETTINGS = {"width": 1920, "height": 1080, "duration_s": 20, "seed": "0xC0FFEE",
            "lighting": "native mission", "fps_cap": 0, "vsync": 0,
            "profile": "stock/default-on native Vulkan", "sim_ticks": 600,
            "corner": bench.CORNER, "capture_trigger": 120, "capture_settle": 30}
ENV = {"MC2_WIDTH": "1920", "MC2_HEIGHT": "1080", "MC2_FPS_CAP": "0", "MC2_VSYNC": "0",
       "MC2_DETERMINISTIC_RNG": "1", "MC2_SMOKE_SEED": "0xC0FFEE", "MC2_RNG_SEED": "0xC0FFEE",
       "MC2_LAUNCHER_ENV_JSON": "0", "MC2_WINDOW_OFFSCREEN": "1"}


def visual():
    return bench.capture  # already loads visual_verify/ab.py as the canonical module


def ab():
    return importlib.import_module("ab")


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def tag_name(tag):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", tag) or ".." in tag:
        raise ValueError("tag must be a single safe archive component")
    return tag


def resolve_tag(tag):
    tag_name(tag)
    sha = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "--verify",
                                   f"refs/tags/{tag}^{{commit}}"], text=True,
                                  **console_child.child_kwargs(redirected=True)).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("tag did not resolve to a commit")
    return sha


def identity(exe, sha):
    binary = bench.stock_identity(exe, sha)
    payload = ab().payload_identity(exe.parent)
    if payload["commit"] != sha:
        raise ValueError("payload manifest does not match historical tag")
    if ab().game_executable(exe.parent).resolve() != exe.resolve():
        raise ValueError("--exe is not the payload's game executable")
    return {"binary": binary, "payload": payload,
            "payload_sha256": digest(payload["payload"]),
            "shader_sha256": digest({k: v for k, v in payload["payload"].items() if k.endswith(".spv")})}


def parity_api():
    try:
        return importlib.import_module("asset_parity")
    except ImportError as exc:
        raise ValueError("asset parity unavailable: land install-asset-parity-1 (2627b321d) before replay") from exc


def parity(exe, reference, expected_reference=None):
    """Read-only full-hash use of the landed helper; never calls sync/require (which may skip)."""
    api = parity_api()
    if not reference.is_dir() or reference.resolve() == exe.parent.resolve():
        raise ValueError("asset reference unavailable or aliases the replay install")
    if reference.resolve() != V04.resolve() and not re.fullmatch(r"[0-9a-f]{64}", expected_reference or ""):
        raise ValueError("a copied v0.4 reference requires its PC --reference-sha256 identity")
    index = {k: v for k, v in api.reference_index(reference).items()
             if not v[0].casefold().endswith(api.SKIP_SUFFIXES)}
    if not index or not any(k.endswith(".fst") for k in index) or not any(k.startswith("data/") for k in index):
        raise ValueError("asset reference is empty or incomplete (data and FST required)")
    rows = [{"path": rel, "bytes": size, "sha256": bench.sha_file(reference / rel)}
            for rel, size in sorted(index.values())]
    ref_sha = digest(rows)
    if expected_reference and ref_sha != expected_reference:
        raise ValueError("v0.4 reference identity mismatch")
    result = api.compare(exe.parent, reference, sample=len(index), sample_bytes=sum(v[1] for v in index.values()) + 1)
    if not result.ok or result.total != len(index) or result.sampled != len(index):
        raise ValueError(f"asset parity failed/incomplete: {api.verdict_line(result, reference)}")
    return {"status": "PASS", "reference": str(reference), "reference_sha256": ref_sha,
            "method": "asset_parity.compare; all reference paths/sizes/SHA256 (no sampling limit)",
            "files": len(rows), "bytes": sum(r["bytes"] for r in rows), "reference_files": rows,
            "exclusions": {"prefixes": list(api.EXCLUDED), "suffixes": list(api.SKIP_SUFFIXES),
                           "scope": "data/, assets/, root *.fst; excludes executable/shaders/mods/runtime dirs"}}


def bookmark_inputs(path):
    owner = json.loads(path.read_text(encoding="utf-8"))
    if owner.get("mission") != "mc2_01" or not owner.get("bookmarks"):
        raise ValueError("owner bookmarks must contain the complete mc2_01 sweep")
    names = set()
    for mark in owner["bookmarks"]:
        name = mark.get("name", "")
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name) or name in names or name in ("corner", "corner_duplicate"):
            raise ValueError("unsafe, duplicate or reserved owner pose name")
        values = [*mark.get("pos", []), mark.get("rot"), mark.get("alt"), mark.get("proj")]
        if len(mark.get("pos", [])) != 3 or any(isinstance(v, bool) or not isinstance(v, (int, float))
                                               or not math.isfinite(v) for v in values):
            raise ValueError("invalid owner pose")
        names.add(name)
    docs = {"mc2_01": {**owner, "bookmarks": [*owner["bookmarks"], dict(bench.CORNER)]}}
    for mission in MISSIONS[1:]:
        docs[mission] = {"schema_v": 1, "mission": mission,
                         "bookmarks": [dict(bench.CORNER), {**bench.CORNER, "name": "corner_duplicate"}]}
    return docs


@dataclass
class Stage:
    kind: str
    mission: str
    command: list[str]
    ref_commit: str
    post_command: list[str] | None = None

    @property
    def key(self):
        return f"{self.kind}/{self.mission}"


def plan(exe, sha, work, docs):
    stages = []
    for mission, doc in docs.items():
        directory = work / "raw/capture" / mission
        cmd = [sys.executable, str(ROOT / "scripts/run_visual_capture.py"), "--exe", str(exe),
               "--mission", mission, "--bookmarks", str(work / "inputs" / f"{mission}.json"),
               "--out-dir", str(directory), "--no-kill", "--show-window", "--trigger-frame", "120", "--settle", "30"]
        for k, v in ENV.items():
            cmd += ["--env-declare", f"{k}={v}"]
        stages.append(Stage("capture", mission, cmd, sha))
    for mission in MISSIONS:
        stages.append(Stage("bench", mission, [sys.executable, str(ROOT / "scripts/run_battle_bench.py"),
            "--stock-corner", mission, "--exe", str(exe), "--ref-commit", sha,
            "--out", str(work / "raw/bench" / mission), "--runs", "1", "--duration", "20", "--resolution", "1080p"], sha))
    for mission in ("mc2_01", "mc2_03"):
        stages.append(Stage("sim", mission, [sys.executable, str(ROOT / "scripts/run_smoke.py"),
            "--mission", mission, "--duration", "30", "--exe", str(exe), "--ref-commit", sha,
            "--keep-logs", "--sim-determinism", "--ticks", "600", "--headless"], sha))
    for mission in MISSIONS:
        directory = work / "raw/zones" / mission
        command = [sys.executable, str(ROOT / "tools/tracy_capture_compare.py"), "capture", "--install", str(exe.parent),
                   "--arm", "vk", "--mission", mission, "--seconds", "20", "--free-timestep", "--fps-cap", "0",
                   "--output", str(directory / "corner.tracy")]
        for k, v in {**ENV, "MC2_TEST_SPAWN": "1", "MC2_UI_COMMAND_FILE": str(directory / "commands.txt"),
                     "MC2_VK_GPU_TIMESTAMPS": "1"}.items():
            command += ["--env", f"{k}={v}"]
        stages.append(Stage("zones", mission, command, sha,
            [sys.executable, str(ROOT / "tools/perf/tracy_table.py"), str(directory), str(directory / "zones.json")]))
    return stages


def host_snapshot():
    now = time.time()
    result = {"timestamp_unix": now, "hostname": platform.node(), "platform": platform.platform(),
              "processor": platform.processor(), "host_jobs": "UNKNOWN", "host_jobs_scope": "no job queue configured",
              "lease_count": "UNKNOWN", "lease_scope": "local smoke_lib host registry",
              "other_game_pids": "UNKNOWN", "pid_scope": "local host, before this stage launches"}
    queue = os.environ.get("MC2_REPLAY_JOB_DIR")
    if queue:
        try:
            result.update(host_jobs=sum(p.suffix in (".json", ".running") for p in Path(queue).iterdir() if p.is_file()),
                          host_jobs_scope=f"{queue}: .json/.running, includes this job")
        except OSError as exc:
            result["host_jobs_error"] = str(exc)
    try:
        status = host_lease.status()
        result.update(lease_count=len(status["slots"]), lease_registry=status,
                      lease_scope=f"local smoke_lib registry {status.get('path', 'UNKNOWN')}; private={status.get('private', 'UNKNOWN')}")
    except Exception as exc:
        result["lease_error"] = str(exc)
    # A redirected lease deliberately hides host PIDs. Query the OS separately;
    # never convert its synthetic empty list into a claimed idle laptop.
    try:
        import psutil
        result["other_game_pids"] = [dict(p.info) for p in psutil.process_iter(["pid", "name", "exe"])
                                     if (p.info.get("name") or "").casefold() in ("mc2.exe", "mc2")]
    except Exception as exc:
        result["pid_error"] = str(exc)
    return result


@contextlib.contextmanager
def replay_environment(reference):
    saved = dict(os.environ)
    try:
        for key in list(os.environ):
            if key.startswith("MC2_") and key != "MC2_VK_DEVICE" and not key.startswith(("MC2_HOST_", "MC2_DEPLOY_", "MC2_CHILD_", "MC2_HARNESS_", "MC2_REPLAY_")):
                del os.environ[key]
        os.environ.update(ENV)
        os.environ["MC2_ASSET_PARITY_REF"] = str(reference)
        os.environ["MC2_HOST_LEASE_TIMEOUT"] = "1200"
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved)


@contextlib.contextmanager
def launch_window(exe, recheck, *, headless=False):
    # Headless run_mission's smoke child acquires its own install lease and
    # rechecks the explicit ref_commit there; it must never inherit our folder hold.
    with host_lease.exclusive_window("tag replay", timeout_s=1200):
        deploy_lease.acquire_lease_waiting(str(exe.parent), "tag replay", 120)
        held = True
        try:
            recheck()
            if headless:
                deploy_lease.release_lease(str(exe.parent))
                held = False
            yield
            if headless:
                deploy_lease.acquire_lease_waiting(str(exe.parent), "tag replay post-child", 120)
                held = True
            recheck()
        finally:
            if held:
                deploy_lease.release_lease(str(exe.parent))


def capture_stage(stage, exe, sha, directory, doc):
    directory.mkdir(parents=True)
    saved_argv = sys.argv
    try:
        sys.argv = stage.command[1:]
        with (directory / "harness.log").open("w", encoding="utf-8") as stream, \
                contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            rc = visual().main()
    finally:
        sys.argv = saved_argv
    if rc:
        raise ValueError(f"capture child failed ({rc}); see {directory}")
    log = directory / f"{stage.mission}_capture.log"
    bench.verify_runtime(log.read_text(encoding="utf-8", errors="replace"), sha)
    for mark in doc["bookmarks"]:
        png = directory / f"{stage.mission}_{mark['name']}.png"
        if not ab().usable_png(png) or not png.with_suffix(".json").is_file():
            raise ValueError(f"missing/invalid PNG or sidecar: {png}")
        if ab().load(png).size != (1920, 1080):
            raise ValueError(f"wrong capture resolution: {png}")
    gpus = ab().capture_gpus(directory)
    if not gpus:
        raise ValueError("capture did not identify its actual GPU")
    return {"status": "PASS", "actual_gpu": gpus}


def sim_summary(log_path, install):
    lines = sim.load_simid(str(log_path))
    if len(lines) < 600 or getattr(lines, "dropped", 0):
        raise ValueError(f"short/invalid SIMID: {len(lines)} observations (need 600), dropped={getattr(lines, 'dropped', 0)}")
    first = lines[:600]
    error = sim.observation_error(first)
    if error:
        raise ValueError(f"invalid SIMID: {error}")
    seed = sim._seed(str(log_path))
    if seed.lower() != SETTINGS["seed"].lower():
        raise ValueError("missing or mismatched SIMID seed")
    fingerprint = sim.data_fingerprint(str(install))
    if not fingerprint.get("digest") or fingerprint.get("problems"):
        raise ValueError("invalid simulation data fingerprint")
    return {"status": "PASS", "claim": "single-run observation hash only; no determinism/save-load verdict",
            "seed": seed, "ticks": [sim._tick(first[0]), sim._tick(first[-1])], "count": 600,
            "chain_schema": 2, "chain": sim.chain(first, 600), "data_fingerprint": fingerprint}


def sim_stage(stage, exe, sha, directory):
    logs = sim.run_mission(stage.mission, "30", str(directory), extra_env={"both": ENV},
                           install=str(exe.parent), ref_commit=sha, runs=1, headless="both", ticks=600)
    if len(logs) != 1:
        raise ValueError("single SIMID run did not return exactly one log")
    bench.verify_runtime(Path(logs[0]).read_text(encoding="utf-8", errors="replace"), sha)
    return sim_summary(Path(logs[0]), exe.parent)


def tracy_tools():
    tracy = importlib.import_module("tracy_capture_compare")
    try:
        tracy.tracy_exe("tracy-capture.exe")
        tracy.tracy_exe("tracy-csvexport.exe")
    except SystemExit as exc:
        return tracy, str(exc)
    return tracy, None


def zones_stage(stage, exe, sha, directory):
    tracy, missing = tracy_tools()
    if missing:
        return {"status": "NOT RUN", "reason": missing}
    channel = bench.prepare_stock(directory)
    trace = directory / "corner.tracy"
    declared = {**ENV, "MC2_TEST_SPAWN": "1", "MC2_UI_COMMAND_FILE": str(channel), "MC2_VK_GPU_TIMESTAMPS": "1"}
    rc = tracy.capture_trace(exe.parent, "vk", stage.mission, 20, trace, fixed_timestep=False,
                             fps_cap=0, show_window=True, extra_env=[f"{k}={v}" for k, v in declared.items()])
    if rc:
        raise ValueError(f"Tracy child failed: {rc}")
    log = trace.with_suffix(".game.log").read_text(encoding="utf-8", errors="replace")
    bench.verify_runtime(log, sha)
    corner = bench.summarize_stock(directory, log)
    table = importlib.import_module("tracy_table")
    saved_argv = sys.argv
    try:
        sys.argv = stage.post_command[1:]
        with (directory / "export.log").open("w", encoding="utf-8") as stream, \
                contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            table.main()
    finally:
        sys.argv = saved_argv
    structured = json.loads((directory / "zones.json").read_text(encoding="utf-8"))
    steady = structured[trace.name]["steady_play"]
    zones = steady["zones"]
    if not zones or any(not isinstance(z.get("self_total_ms"), (int, float))
                        or not math.isfinite(z["self_total_ms"]) or z["self_total_ms"] < 0 for z in zones):
        raise ValueError("invalid Tracy structured zones")
    gpus = sorted({p.device for p in vk_profile.parse(log)})
    if not gpus:
        raise ValueError("Tracy run did not identify its actual GPU")
    return {"status": "PASS", "attribution": {"sha": sha, "mission": stage.mission,
            "trace_sha256": bench.sha_file(trace), "pose": corner["acknowledged_pose"]},
            "phase": {k: v for k, v in steady.items() if k != "zones"}, "actual_gpu": gpus,
            "top_ten": sorted(zones, key=lambda z: -z["self_total_ms"])[:10],
            "scope": "same-tag separate corner run, steady_play after 60 active frames; includes waits"}


def artifact_hashes(directory):
    return {p.relative_to(directory).as_posix(): bench.sha_file(p) for p in sorted(directory.rglob("*"))
            if p.is_file() and p != directory / "manifest.json"}


def required_stages():
    return [f"{kind}/{mission}" for kind in ("capture", "bench") for mission in MISSIONS] + ["sim/mc2_01", "sim/mc2_03"]


def validate_bundle(directory):
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != "tag-replay/1" or manifest.get("status") != "complete":
        raise ValueError("incomplete replay bundle")
    sha = manifest.get("resolved_sha", "")
    provenance = bench.ms.binary_provenance(manifest.get("identity", {}).get("binary"), {"commit": sha})
    if (not re.fullmatch(r"[0-9a-f]{40}", sha) or not provenance.get("available")
            or provenance.get("dirty") is not False or not provenance.get("source_head_matches_binary")
            or manifest.get("identity", {}).get("payload", {}).get("commit") != sha):
        raise ValueError("missing or mismatched bundled provenance")
    if manifest.get("parity", {}).get("status") != "PASS" or not re.fullmatch(
            r"[0-9a-f]{64}", manifest.get("parity", {}).get("reference_sha256", "")):
        raise ValueError("missing bundled asset parity/reference identity")
    if not manifest.get("host", {}).get("actual_gpu"):
        raise ValueError("missing bundled GPU identity")
    if any(manifest["stages"].get(k, {}).get("status") != "PASS" for k in required_stages()):
        raise ValueError("incomplete required stages")
    if any(manifest["stages"].get(f"zones/{m}", {}).get("status") not in ("PASS", "NOT RUN") for m in MISSIONS):
        raise ValueError("incomplete Tracy stages")
    for mission in MISSIONS:
        cpu = manifest["stages"][f"bench/{mission}"]["cpu_work"]
        if not isinstance(cpu.get("n"), int) or isinstance(cpu["n"], bool) or cpu["n"] < 1:
            raise ValueError("invalid bundled CPU sample count")
        for key in ("over_6_06_ms", "over_8_33_ms"):
            if not isinstance(cpu.get(key), int) or isinstance(cpu[key], bool) or not 0 <= cpu[key] <= cpu["n"]:
                raise ValueError("invalid bundled CPU threshold count")
        if cpu["over_8_33_ms"] > cpu["over_6_06_ms"] or any(
                not isinstance(cpu.get(k), (int, float)) or isinstance(cpu[k], bool)
                or not math.isfinite(cpu[k]) or cpu[k] < 0 for k in ("p99_ms", "max_ms")) or cpu["p99_ms"] > cpu["max_ms"]:
            raise ValueError("invalid bundled CPU distribution")
    for mission in ("mc2_01", "mc2_03"):
        row = manifest["stages"][f"sim/{mission}"]
        if row.get("count") != 600 or row.get("chain_schema") != 2 or row["ticks"][1] - row["ticks"][0] != 599:
            raise ValueError("invalid bundled SIMID range/schema")
    if manifest.get("artifacts") != artifact_hashes(directory):
        raise ValueError("bundle artifact hashes missing, added or mismatched")
    for name in ("frames.md", "frames.json", "simhash.txt"):
        if name not in manifest["artifacts"]:
            raise ValueError(f"missing required artifact {name}")
    docs = manifest["bookmarks"]
    for mission in MISSIONS:
        capture_dir = f"raw/capture/{mission}"
        required = [f"{capture_dir}/{mission}_capture.log", f"{capture_dir}/{mission}_capture_manifest.json"]
        required += [f"raw/bench/{mission}/{name}" for name in
                     ("frames.jsonl", "engine.log", "environment.json", "stock-corner.json", "commands.txt.ack")]
        for mark in docs[mission]["bookmarks"]:
            required += [f"{capture_dir}/{mission}_{mark['name']}.{ext}" for ext in ("png", "json")]
            if mark["name"] == "corner_duplicate":
                continue
            path = directory / "png" / f"{mission}_{mark['name']}.png"
            if not ab().usable_png(path):
                raise ValueError(f"missing PNG {path.name}")
        if manifest["stages"][f"zones/{mission}"]["status"] == "PASS":
            required += [f"raw/zones/{mission}/{name}" for name in
                         ("corner.tracy", "corner.game.log", "zones.json", "frames.jsonl", "commands.txt.ack")]
        for rel in required:
            if rel not in manifest["artifacts"]:
                raise ValueError(f"missing raw evidence: {rel}")
    for mission in ("mc2_01", "mc2_03"):
        if f"raw/sim/{mission}/a/{mission}.log" not in manifest["artifacts"]:
            raise ValueError(f"missing raw SIMID log: {mission}")
    return manifest


def publish(work, destination):
    validate_bundle(work)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise ValueError(f"immutable output already exists: {destination}")
    if work.stat().st_dev != destination.parent.stat().st_dev:
        raise ValueError("publication needs scratch and output on the same volume (atomic rename)")
    # Windows rename fails if ANY destination exists, including an empty dir.
    os.rename(work, destination)


def write_reports(work, manifest, pc_sweep):
    frames = {m: manifest["stages"][f"bench/{m}"] for m in MISSIONS}
    dump(work / "frames.json", frames)
    lines = ["CPU work times before the FPS cap (disabled), not measured display intervals.",
             "", "| mission | n | >6.06 ms | >8.33 ms | nearest-rank p99 ms | max ms |",
             "|---|---:|---:|---:|---:|---:|"]
    for mission, row in frames.items():
        cpu = row["cpu_work"]
        lines.append(f"| {mission} | {cpu['n']} | {cpu['over_6_06_ms']} | {cpu['over_8_33_ms']} | {cpu['p99_ms']} | {cpu['max_ms']} |")
    (work / "frames.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    hashes = {m: manifest["stages"][f"sim/{m}"] for m in ("mc2_01", "mc2_03")}
    (work / "simhash.txt").write_text("Single-run observations only; no determinism/save-load PASS claim.\n"
                                         + json.dumps(hashes, indent=2) + "\n", encoding="utf-8")
    comparison = ["Laptop vs PC corner sweep (diagnostic; cross-host timings are not regression verdicts).", "",
                  "| mission | laptop CPU steady Frame mean ms | PC CPU whole-capture mean ms | CPU delta | GPU delta |",
                  "|---|---:|---:|---|---|"]
    pc_means = {}
    if pc_sweep.is_file():
        text = pc_sweep.read_text(encoding="utf-8")
        shutil.copyfile(pc_sweep, work / "pc-sweep.md")
        section = text.split("### Corner (owner pose, whole capture)", 1)[-1].split("### Battle window", 1)[0]
        for line in section.splitlines():
            fields = [f.strip() for f in line.split("|")]
            if len(fields) > 5 and fields[1] in MISSIONS:
                pc_means[fields[1]] = float(fields[4])
    for mission in MISSIONS:
        zone = manifest["stages"][f"zones/{mission}"]
        mean = (zone.get("phase", {}).get("frame_latency") or {}).get("mean_ms", "NOT RUN")
        comparison.append(f"| {mission} | {mean} | {pc_means.get(mission, 'UNKNOWN')} | INCOMPATIBLE: sampling windows | INCOMPATIBLE: PC GPU is 24-map aggregate |")
    (work / "laptop-vs-pc.md").write_text("\n".join(comparison) + "\n", encoding="utf-8")


def run(args):
    started = time.monotonic()
    tag = tag_name(args.tag)
    sha = resolve_tag(tag)
    destination = args.out_root.resolve() / tag
    if destination.exists():
        raise ValueError(f"immutable output already exists: {destination}")
    docs = bookmark_inputs(args.bookmarks)
    work = args.scratch_root.resolve() / f"{tag}-<fresh>"
    if args.dry_run:
        print(json.dumps({"tag": tag, "resolved_sha": sha, "destination": str(destination), "scratch": str(work),
            "environment": ENV, "generated_corner_inputs": docs,
            "generated_command_files": {str(work / "raw" / kind / mission / "commands.txt"):
                bench.stock_commands(work / "raw" / kind / mission) for kind in ("bench", "zones") for mission in MISSIONS},
            "stages": [vars(s) for s in plan(args.exe, sha, work, docs)],
            "preflights": ["clean embedded binary at resolved SHA; verified deployed executable/SPIR-V manifest",
                "asset_parity dependency present; read-only full-hash v0.4 parity (no skip)",
                "copied reference: --reference-sha256 from PC v0.4 snapshot identity",
                "exclusive host then install; bounded 1200s/120s waits; recheck identity/parity after waits",
                "headless child takes its own install lease and explicit --ref-commit; verify again after child",
                "existing Tracy recorder/exporter or zones NOT RUN; no install/build",
                "scratch/output same volume; absent tag dir; complete bundle validation before atomic publication"],
            "artifacts": ["frames.md", "frames.json", "png/<mission>_<pose>.png", "simhash.txt", "manifest.json", "raw/**", "laptop-vs-pc.md"]}, indent=2))
        # Dry run is read-only: preflight identity/parity, but never acquires a lease.
        actual = identity(args.exe, sha)
        checked = parity(args.exe, args.asset_reference, args.reference_sha256)
        print(json.dumps({"preflight": "PASS", "binary_sha256": actual["binary"]["sha256"],
                          "payload_sha256": actual.get("payload_sha256"), "shader_sha256": actual.get("shader_sha256"),
                          "parity": {k: v for k, v in checked.items() if k != "reference_files"}}, indent=2))
        return 0
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f"{tag}-", dir=args.scratch_root)).resolve()
    manifest = {"schema": "tag-replay/1", "status": "incomplete", "tag": tag, "resolved_sha": sha,
                "host": host_snapshot(), "settings": SETTINGS, "bookmarks": docs,
                "owner_bookmarks_sha256": bench.sha_file(args.bookmarks), "stages": {}, "started_unix": time.time()}
    try:
        original = identity(args.exe, sha)
        checked = parity(args.exe, args.asset_reference, args.reference_sha256)
        manifest.update(identity=original, parity=checked)
        def recheck():
            if identity(args.exe, sha) != original:
                raise ValueError("binary/payload identity changed during lease wait/run")
            now = parity(args.exe, args.asset_reference, checked["reference_sha256"])
            if now != checked:
                raise ValueError("asset parity/reference changed during lease wait/run")
        (work / "inputs").mkdir()
        (work / "png").mkdir()
        for mission, doc in docs.items():
            dump(work / "inputs" / f"{mission}.json", doc)
        with replay_environment(args.asset_reference):
            for stage in plan(args.exe, sha, work, docs):
                tick = time.monotonic()
                record = {"status": "incomplete", "command": stage.command, "ref_commit": stage.ref_commit, "before": host_snapshot()}
                manifest["stages"][stage.key] = record
                dump(work / "incomplete.json", manifest)
                directory = work / "raw" / stage.kind / stage.mission
                directory.parent.mkdir(parents=True, exist_ok=True)
                if stage.kind == "zones":
                    _, missing = tracy_tools()
                    if missing:
                        record.update(status="NOT RUN", reason=missing, wall_s=time.monotonic() - tick, after=host_snapshot())
                        dump(work / "incomplete.json", manifest)
                        continue
                with launch_window(args.exe, recheck, headless=stage.kind == "sim"):
                    if stage.kind == "capture":
                        result = capture_stage(stage, args.exe, sha, directory, docs[stage.mission])
                        for mark in docs[stage.mission]["bookmarks"]:
                            if mark["name"] != "corner_duplicate":
                                name = f"{stage.mission}_{mark['name']}.png"
                                shutil.copyfile(directory / name, work / "png" / name)
                    elif stage.kind == "bench":
                        result = {"status": "PASS", **bench.stock_run(args.exe, directory, stage.mission, sha)}
                    elif stage.kind == "sim":
                        result = sim_stage(stage, args.exe, sha, directory)
                    else:
                        result = zones_stage(stage, args.exe, sha, directory)
                record.update(result, wall_s=time.monotonic() - tick, after=host_snapshot())
                dump(work / "incomplete.json", manifest)
        gpus = [manifest["stages"][f"capture/{m}"]["actual_gpu"] for m in MISSIONS]
        if any(g != gpus[0] for g in gpus):
            raise ValueError("actual GPU changed between captures")
        for key, record in manifest["stages"].items():
            if key.startswith(("bench/", "zones/")) and record["status"] == "PASS" and record.get("actual_gpu") != gpus[0]:
                raise ValueError(f"actual GPU changed for {key}")
        manifest["host"]["actual_gpu"] = gpus[0]
        manifest["content"] = bench.content_identity(args.exe.parent)
        manifest["render_options"] = bench.render_options_pin.record(args.exe.parent, ENV)["values"]
        write_reports(work, manifest, args.pc_sweep)
        manifest.update(status="complete", wall_s=time.monotonic() - started)
        # Keep the progress record as an explicit historical diagnostic, not a verdict.
        manifest["artifacts"] = artifact_hashes(work)
        dump(work / "manifest.json", manifest)
        publish(work, destination)
        print(f"complete: {destination}; wall_s={manifest['wall_s']:.1f}")
        return 0
    except (Exception, SystemExit) as exc:
        manifest.update(status="incomplete", error=str(exc), wall_s=time.monotonic() - started)
        dump(work / "incomplete.json", manifest)
        print(f"incomplete evidence retained: {work}; {exc}", file=sys.stderr)
        return 1


def compare(out_root, tags):
    directories = [out_root / tag_name(t) for t in tags]
    # Report missing pairs even when the bundle has been damaged.
    manifests, problems = [], []
    for directory in directories:
        try:
            manifests.append(validate_bundle(directory))
        except (ValueError, OSError, KeyError) as exc:
            problems.append(f"{directory.name}: incomplete: {exc}")
            try:
                manifests.append(json.loads((directory / "manifest.json").read_text(encoding="utf-8")))
            except (OSError, ValueError):
                manifests.append({})
    a, b = manifests
    mismatches = [key for key in ("settings", "bookmarks", "owner_bookmarks_sha256", "render_options") if a.get(key) != b.get(key)]
    if a.get("parity", {}).get("reference_sha256") != b.get("parity", {}).get("reference_sha256"):
        mismatches.append("asset reference/content")
    if a.get("content", {}).get("sha256") != b.get("content", {}).get("sha256"):
        mismatches.append("installed content")
    for mission in ("mc2_01", "mc2_03"):
        sa, sb = (m.get("stages", {}).get(f"sim/{mission}", {}) for m in manifests)
        if any(sa.get(k) != sb.get(k) for k in ("seed", "ticks", "count", "chain_schema", "data_fingerprint")):
            mismatches.append(f"simulation inputs/{mission}")
    if any(a.get("host", {}).get(k) != b.get("host", {}).get(k) for k in ("hostname", "platform", "processor", "actual_gpu")):
        mismatches.append("host/GPU")
    metrics_ok = not problems and not mismatches
    deltas = {}
    if metrics_ok:
        for mission in MISSIONS:
            before = a["stages"][f"bench/{mission}"]["cpu_work"]
            after = b["stages"][f"bench/{mission}"]["cpu_work"]
            deltas[mission] = {key: after[key] - before[key] for key in
                               ("n", "over_6_06_ms", "over_8_33_ms", "p99_ms", "max_ms")}
            deltas[mission]["sampling_a"] = a["stages"][f"bench/{mission}"].get("sampling")
            deltas[mission]["sampling_b"] = b["stages"][f"bench/{mission}"].get("sampling")
    names = sorted({p.name for d in directories for p in (d / "png").glob("*.png")})
    for manifest in manifests:
        for mission, doc in manifest.get("bookmarks", {}).items():
            names += [f"{mission}_{m['name']}.png" for m in doc["bookmarks"] if m["name"] != "corner_duplicate"]
    pairs = []
    for name in sorted(set(names)):
        pa, pb = (d / "png" / name for d in directories)
        if not ab().usable_png(pa) or not ab().usable_png(pb):
            pairs.append({"png": name, "status": "MISSING/INVALID", "a": pa.is_file(), "b": pb.is_file()})
            continue
        ia, ib = ab().load(pa), ab().load(pb)
        if ia.size != ib.size:
            pairs.append({"png": name, "status": "INCOMPATIBLE dimensions"})
            continue
        np = ab().np
        pixels_a, pixels_b = np.asarray(ia).astype("int16"), np.asarray(ib).astype("int16")
        changed = int(np.any(pixels_a != pixels_b, axis=2).sum())
        pairs.append({"png": name, "status": "DIFFERENT" if changed else "IDENTICAL decoded pixels",
                      "changed_pixels": changed, "mean_absolute_channel_delta": float(np.abs(pixels_a - pixels_b).mean())})
    print(json.dumps({"tags": tags, "metrics": "COMPATIBLE" if metrics_ok else "INCOMPATIBLE",
                      "reasons": problems + mismatches, "frame_deltas": deltas, "png_pairs": pairs,
                      "note": "decoded-pixel differences are not visual acceptance verdicts"}, indent=2))
    return 0 if metrics_ok and pairs and all(p["status"] not in ("MISSING/INVALID", "INCOMPATIBLE dimensions") for p in pairs) else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--tag")
    mode.add_argument("--compare", nargs=2, metavar=("TAG_A", "TAG_B"))
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--bookmarks", type=Path, default=DEFAULT_BOOKMARKS)
    parser.add_argument("--asset-reference", type=Path)
    parser.add_argument("--reference-sha256", help="PC v0.4 asset identity; required for a host-local reference copy")
    parser.add_argument("--out-root", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--scratch-root", type=Path, default=Path("E:/mc2-tmp/slot05/tag-replay"))  # drive-path: ok assigned slot scratch; laptop overrides explicitly
    parser.add_argument("--pc-sweep", type=Path, default=Path("G:/mc2-tmp/perf-brief/stats.md"))  # drive-path: ok binding owner reference; --pc-sweep supplies laptop copy
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not args.compare and (not args.exe or not args.asset_reference):
        parser.error("--tag requires --exe and --asset-reference")
    try:
        if args.compare:
            return compare(args.out_root, args.compare)
        args.exe = args.exe.resolve()
        args.asset_reference = args.asset_reference.resolve()
        return run(args)
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        print(f"tag replay refused: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

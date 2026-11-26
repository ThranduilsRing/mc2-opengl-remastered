#!/usr/bin/env python3
"""Warm a deployed build's Vulkan pipelines through rendering smoke runs."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts import deploy_payload
from scripts.smoke_lib import host_lease, nofocus, render_options_pin


def launcher_env(install: Path, inherited: dict) -> dict:
    env = dict(inherited)
    path = install / "launcher_env.json"
    settings = json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {}
    options = []
    for key, value in settings.items():
        if not isinstance(value, (str, int, float, bool)):
            continue
        text = str(int(value)) if isinstance(value, bool) else str(value)
        if key.startswith("MC2_"):
            env.setdefault(key, text)
        elif key in render_options_pin.keys():
            options.append(f"{key}={text}")
    # Existing explicit overrides retain the same precedence as a launcher run.
    env["MC2_OPTIONS_OVERRIDE"] = ",".join(options + [env.get("MC2_OPTIONS_OVERRIDE", "")])
    env.update(MC2_DIAG_VERBOSE="1", MC2_SMOKE_FIXED_TIMESTEP="0")
    env.pop("MC2_HEADLESS", None)
    return nofocus.apply_env(env)


def require_idle(install: Path) -> None:
    exe = install / "mc2.exe"
    for pid, path in deploy_payload._enumerate_matching_processes("mc2.exe"):
        if path is None or deploy_payload._same_path(path, str(exe)):
            raise RuntimeError(f"refusing to warm: mc2.exe pid={pid} is running from this install or its path is unreadable")


def parse_metrics(text: str) -> tuple[str, float]:
    status = re.search(r"\[VK_PIPECACHE\] status=(\w+)", text)
    creation = re.findall(r"\[VK_LOAD_RESOURCE v1\] pipeline_creation_ms=([\d.]+)", text)
    if not status or not creation or not re.search(r"\[TIMING v1\] event=mission_ready\b", text):
        raise RuntimeError("missing pipeline status, creation time, or mission_ready")
    return status[1], sum(map(float, creation))


def cache_bytes(install: Path) -> int:
    files = list((install / "cache" / "vulkan").glob("*.bin"))
    exe_time = (install / "mc2.exe").stat().st_mtime_ns
    fresh = [p for p in files if p.stat().st_mtime_ns > exe_time and p.stat().st_size > 0]
    if not fresh:
        raise RuntimeError("no nonempty Vulkan cache newer than mc2.exe")
    return sum(p.stat().st_size for p in fresh)


def warm(install: Path, missions: list[str]) -> None:
    install = install.resolve(strict=True)
    if not (install / "mc2.exe").is_file():
        raise RuntimeError("install has no mc2.exe")
    if not missions or any(not re.fullmatch(r"[A-Za-z0-9_-]+", m) for m in missions):
        raise RuntimeError("missions must be comma-separated mission stems")
    require_idle(install)
    env = launcher_env(install, os.environ)
    tag = f"pipeline-warm-{os.getpid()}"
    held = False
    host_lease.hold(tag, ttl_s=1800, timeout_s=600)
    try:
        env[host_lease.BATCH_ENV] = tag
        held = deploy_payload.take_folder_lease(str(install), 0)
        require_idle(install)
        receipts = install / "pipeline-warm" / str(time.time_ns())
        first_status, first_ms = "", 0.0
        for i, mission in enumerate(missions):
            artifact_root = receipts / mission
            child_env = dict(env, MC2_SMOKE_ARTIFACT_ROOT=str(artifact_root))
            # --lite cannot select missions; this is its ordinary rendering runner path.
            command = [sys.executable, "-B", str(ROOT / "scripts/run_smoke.py"),
                       "--exe", str(install / "mc2.exe"), "--mission", mission,
                       "--duration", "2", "--keep-logs", "--no-lease", "--wait-timeout", "600"]
            result = subprocess.run(command, cwd=ROOT, env=child_env, capture_output=True,
                                    text=True, timeout=780, **nofocus.hidden_kwargs())
            artifact_root.mkdir(parents=True, exist_ok=True)
            (artifact_root / "runner.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError(f"smoke failed ({result.returncode}); see {artifact_root}")
            logs = list(artifact_root.rglob(f"{mission}.log"))
            if len(logs) != 1:
                raise RuntimeError(f"expected one {mission}.log in {artifact_root}")
            status, ms = parse_metrics(logs[0].read_text(encoding="utf-8", errors="replace"))
            if status not in ({"warm", "cold", "rejected"} if i == 0 else {"warm"}):
                raise RuntimeError(f"{mission}: unexpected pipeline cache status={status}")
            cache_bytes(install)
            if i == 0:
                first_status, first_ms = status, ms
        print(f"[PIPELINE_WARM v1] install={install} status_before={first_status} "
              f"creation_ms_first={first_ms:.3f} cache_bytes={cache_bytes(install)}")
    finally:
        if held:
            deploy_payload.release_folder_lease(str(install))
        host_lease.release_batch(tag)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("install", type=Path)
    parser.add_argument("--missions", default="mc2_01,mc2_10")
    args = parser.parse_args(argv)
    try:
        warm(args.install, args.missions.split(","))
    except (RuntimeError, OSError, subprocess.SubprocessError, host_lease.HostLeaseBusy) as exc:
        print(f"[PIPELINE_WARM v1] FAIL {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

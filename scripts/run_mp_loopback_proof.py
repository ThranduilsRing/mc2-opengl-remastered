#!/usr/bin/env python3
"""Off-screen ENet co-op proof. Trigger: code/net/, tactical_order_queue.*, mission_update.cpp.

Both peers are headless by default; --render host/client/both adds off-screen
Vulkan captures under the existing GPU lease. Never requires the visible desktop.
"""
from __future__ import annotations
import argparse
import base64
import html
import json
from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import time

from smoke_lib import host_lease, nofocus
from smoke_lib.runner import FORCED_ENV, SOFT_ENV, POPPED_ENV


def read_trace(path: Path) -> tuple[dict[int, tuple[str, ...]], list[str]]:
    hashes, orders = {}, []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields and fields[0] == "H":
            if len(fields) != 7 or int(fields[1]) != len(hashes) + 1:
                raise ValueError(f"missing, duplicated or malformed hash: {line}")
            hashes[int(fields[1])] = tuple(fields[2:])
        elif fields and fields[0] == "C":
            orders.append(line)
    return hashes, orders


def compare_traces(host: Path, client: Path, ticks: int, forced: int = 0) -> dict:
    a, commands_a = read_trace(host / "session.trace")
    b, commands_b = read_trace(client / "session.trace")
    expected = forced or ticks
    if len(a) != expected or len(b) != expected:
        raise ValueError(f"incomplete evidence: host={len(a)} client={len(b)} required={expected}")
    different = [tick for tick in a if a[tick] != b[tick]]
    if different != ([forced] if forced else []):
        raise ValueError(f"first divergent tick: {different[:1]}; expected {forced or 'none'}")
    if commands_a != commands_b or not commands_a:
        raise ValueError("peer command traces differ or no scripted orders were applied")
    for peer in (host, client):
        text = (peer / "session.trace").read_text(encoding="utf-8", errors="replace")
        marker = f"[MP_DESYNC] tick={forced} "
        if (forced and marker not in text) or (not forced and "[MP_DESYNC]" in text):
            raise ValueError(f"unexpected/missing desync verdict in {peer}")
    return {"ticks": expected, "commands": len(commands_a), "first_divergent_tick": forced or None}


def peer_start(install: str, directory: str, slot: int, port: int, ticks: int,
               timeout: int, rendered: bool, logs: ExitStack):
    root = Path(directory)
    env = os.environ.copy()
    env.update(FORCED_ENV)
    for key, value in SOFT_ENV.items():
        env.setdefault(key, value)
    for key in POPPED_ENV:
        env.pop(key, None)
    for key in ("MC2_COMMAND_REPLAY_PLAY", "MC2_COMMAND_REPLAY_RECORD"):
        env.pop(key, None)
    env.update(MC2_SMOKE_FIXED_TIMESTEP="1", MC2_SIM_IDENTITY_TRACE="1", MC2_FORCE_ALL_VISIBLE="0",
               MC2_DETERMINISTIC_RNG="1", MC2_RNG_SEED="12345",
               MC2_DIAGNOSTIC_TRACE_FILE=str(root / "diagnostic.jsonl"),
               MC2_SAVE_DIR=str(root / "saves"),
               MC2_CAMERA_PARK=("-1500,2500,0,60,800@30" if slot == 0 else "-2000,2900,90,50,600@30"))
    if rendered:
        env.update(MC2_VISUAL_CAPTURE_FRAME="600", MC2_VISUAL_CAPTURE_DIR=str(root))
    argv = [str(Path(install) / "mc2.exe"), "--mission", "mc2_01", "--profile", "tier1",
            "--duration", str(timeout), "--ticks", str(ticks),
            "--host" if slot == 0 else "--join", f"127.0.0.1:{port}",
            "--mp-user-dir", str(root)]
    if not rendered:
        argv.append("--headless")
    log = logs.enter_context((root / "launcher.log").open("w", encoding="utf-8"))
    return nofocus.spawn(argv, env=env, cwd=install, stdout=log, stderr=subprocess.STDOUT)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--ticks", type=int, default=18000)
    ap.add_argument("--port", type=int, default=23460)
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--render", choices=("none", "host", "client", "both"), default="none")
    ap.add_argument("--force-desync", type=int, default=0)
    args = ap.parse_args()
    if args.ticks < 64 or not 0 <= args.force_desync <= args.ticks or not 1 <= args.port <= 65535:
        ap.error("need ticks>=64, 0<=force-desync<=ticks and a valid port")
    args.install = args.install.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    peers = [args.out / "host", args.out / "client"]
    for slot, directory in enumerate(peers):
        directory.mkdir()
        # Different lances send commands throughout the run, not only at startup.
        script = ["0 0 delay 1"] if slot else []
        script.append(f"0 {1 - slot} reject")
        for tick in range(0, args.ticks - 4, 300):
            script.append(f"{tick} {slot} move {-1600 + (tick // 300 % 4) * 160} {2500 + slot * 100}")
        if args.force_desync and slot:
            script.append(f"{args.force_desync} 0 desync")
        (directory / "orders.txt").write_text("\n".join(script) + "\n", encoding="utf-8")
    processes = []
    started = time.monotonic()
    try:
        with ExitStack() as logs:
            if args.render != "none":
                host_lease.acquire("mp loopback proof", timeout_s=120)
            for slot, directory in enumerate(peers):
                rendered = args.render == "both" or args.render == ("host" if slot == 0 else "client")
                processes.append(peer_start(str(args.install), str(directory), slot, args.port,
                    args.ticks, args.timeout, rendered, logs))
            for directory, proc in zip(peers, processes):
                rc = proc.wait(timeout=max(1, args.timeout - (time.monotonic() - started)))
                (directory / "exit.json").write_text(json.dumps({"rc": rc, "pid": proc.pid}), encoding="utf-8")
            if any(proc.returncode != (3 if args.force_desync else 0) for proc in processes):
                raise ValueError("a peer failed; see peer launcher/stderr logs")
        result = compare_traces(*peers, args.ticks, args.force_desync)
        result.update(result="PASS", wall_seconds=round(time.monotonic() - started, 2))
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        result = {"result": "FAIL", "error": str(error)}
    finally:
        for proc in processes:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        host_lease.release()
    (args.out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    images = []
    for directory in peers:
        for png in directory.glob("*.png"):
            images.append(f'<h2>{directory.name}</h2><img style="max-width:100%" alt="{directory.name}" '
                          f'src="data:image/png;base64,{base64.b64encode(png.read_bytes()).decode()}">')
    (args.out / "RESULT.html").write_text('<!doctype html><meta charset="utf-8"><title>MC2 co-op proof</title>'
        + '<pre>' + html.escape(json.dumps(result, indent=2)) + '</pre>' + ''.join(images)
        + ("<p>Off-screen screenshots NOT RUN (headless protocol/combat proof).</p>" if not images else ""), encoding="utf-8")
    print(json.dumps(result))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

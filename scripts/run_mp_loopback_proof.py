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
import hashlib
import heapq
import ipaddress
import random
import select
import socket
from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import time

from smoke_lib import host_lease, nofocus, deploy_lease
from smoke_lib.runner import FORCED_ENV, SOFT_ENV, POPPED_ENV, unregistered_ambient


def read_trace(path: Path) -> tuple[dict[int, tuple[str, ...]], list[str]]:
    hashes, orders = {}, []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields and fields[0] == "H":
            if len(fields) != 7 or int(fields[1]) != len(hashes) + 1:
                raise ValueError(f"missing, duplicated or malformed hash: {line}")
            hashes[int(fields[1])] = tuple(fields[2:])
        elif fields and fields[0] in ("C", "R"):
            orders.append(line)
    return hashes, orders


def compare_traces(host: Path, client: Path, ticks: int, forced: int = 0, combat: bool = False,
                   max_extra: int = 2) -> dict:
    a, commands_a = read_trace(host / "session.trace")
    b, commands_b = read_trace(client / "session.trace")
    expected = forced or ticks
    extra = 0 if forced else max_extra
    if not expected <= len(a) <= expected + extra or not expected <= len(b) <= expected + extra:
        raise ValueError(f"incomplete evidence: host={len(a)} client={len(b)} required={expected}")
    common = min(len(a), len(b))
    different = [tick for tick in range(1, common + 1) if a[tick] != b[tick]]
    if different != ([forced] if forced else []):
        raise ValueError(f"first divergent tick: {different[:1]}; expected {forced or 'none'}")
    commands_a = [line for line in commands_a if int(line.split()[1]) <= common]
    commands_b = [line for line in commands_b if int(line.split()[1]) <= common]
    if commands_a != commands_b or not commands_a:
        raise ValueError("peer command traces differ or no scripted orders were applied")
    for peer in (host, client):
        text = (peer / "session.trace").read_text(encoding="utf-8", errors="replace")
        marker = f"[MP_DESYNC] tick={forced} "
        if (forced and marker not in text) or (not forced and "[MP_DESYNC]" in text):
            raise ValueError(f"unexpected/missing desync verdict in {peer}")
    rng_states = len({a[tick][-1] for tick in range(1, expected + 1)})
    evidence = []
    if combat:
        for peer in (host, client):
            rows = [line.split() for line in (peer / "session.trace").read_text().splitlines() if line.startswith("B ")]
            counts = {f"{slot}:{event}": sum(int(r[1]) <= common and r[2] == str(slot) and r[3] == event
                      and (event == "fire" or float(r[6]) > 0) for r in rows)
                      for slot in (0, 1) for event in ("fire", "damage")}
            if not all(counts.values()):
                raise ValueError(f"combat requires firing and damage from both owned groups: {counts}")
            evidence.append(counts)
    return {"ticks": expected, "equal_ticks": common - bool(forced), "rng_states": rng_states,
            "combat": evidence, "uncompared_tail_ticks": [len(a) - common, len(b) - common],
            "commands": len(commands_a), "first_divergent_tick": forced or None}


def fixture_orders(slot: int, ticks: int, forced: int, combat: bool = False) -> list[str]:
    script = ["0 0 delay 1"] if slot else []
    script.append(f"0 {1 - slot} reject")
    if combat:
        # Stock mc2_01 bandit mechs, identified by authored Part ID.
        script.extend(f"0 {mover} attack {704 + slot}" for mover in ((1, 2) if slot else (0,)))
        return script
    # Stay by the stock deployment, south of objective 0's airfield trigger.
    # The old positive-Y route walked all three mechs into the bandit force.
    for tick in range(0, ticks - 4, 300):
        for mover in ((1, 2) if slot else (0,)):
            x, y = (2800 + mover * 150, 1900) if combat else (2400 + (tick // 300 % 4) * 50, -2000 - mover * 100)
            script.append(f"{tick} {mover} move {x} {y}")
    if forced and slot:
        script.append(f"{forced} 0 desync")
    return script


def peer_start(install: str, directory: str, slot: int, port: int, ticks: int,
               timeout: int, rendered: bool, logs: ExitStack, address: str = "127.0.0.1", combat: bool = False,
               wall_clock: bool = False, lobby: bool = False):
    root = Path(directory)
    env = os.environ.copy()
    for key in unregistered_ambient(env):
        env.pop(key, None)
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
               MC2_CAMERA_PARK=("2450,-2100,0,60,1200@30" if slot == 0 else "2450,-2100,90,50,1200@30"))
    if wall_clock:
        # Presence gate: even the string "0" enables the virtual clock.
        env.pop("MC2_SMOKE_FIXED_TIMESTEP", None)
    if combat:
        env["MC2_CAMERA_PARK"] = "3000,1900,0,60,1700@30"
    if rendered:
        env.update(MC2_VISUAL_CAPTURE_FRAME=str(min(2400, ticks // 4)), MC2_VISUAL_CAPTURE_DIR=str(root))
    argv = [str(Path(install) / "mc2.exe"), "--mission", "mc2_01", "--profile", "tier1",
            "--duration", str(ticks // 30 + 10 if wall_clock else timeout), "--ticks", str(ticks),
            "--host" if slot == 0 else "--join", f"{address}:{port}",
            "--mp-user-dir", str(root)]
    if not rendered:
        argv.append("--headless")
    if lobby:
        # Enter through the menu and real widgets; never pass --host/--join.
        option = argv.index("--host" if slot == 0 else "--join")
        del argv[option:option + 2]
        env.update(MC2_BOOT_TO_SCREEN="mainmenu", MC2_MENU_CANARY_SKIP_INTRO="1",
                   MC2_DIAG_TAGS="COOP_UI",
                   MC2_WIDTH="1920", MC2_HEIGHT="1080", MC2_INPUT_SCRIPT=str(root / "lobby.script"))
        # run_pair appends clicks at the real controls' reported hit regions.
        script = ["wait 180"]
        (root / "lobby.script").write_text("\n".join(script) + "\n", encoding="utf-8")
    log = logs.enter_context((root / "launcher.log").open("w", encoding="utf-8"))
    return nofocus.spawn(argv, env=env, cwd=install, stdout=log, stderr=subprocess.STDOUT)


class LobbyDriver:
    """Advance real mouse scripts only as the menu exposes enabled controls."""
    def __init__(self, peers):
        self.peers = peers
        self.steps = [0, 0]
        self.listening = False
        self.actions = []

    def pump(self):
        for slot, root in enumerate(self.peers):
            path = root / "stderr.log"
            text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
            self.listening |= slot == 0 and "[COOP_UI] Listening " in text
            names = ["Multiplayer", "Host" if slot == 0 else "Join", "Ready"] + (["Start"] if slot == 0 else [])
            step = self.steps[slot]
            if step == len(names) or (slot == 1 and step == 1 and not self.listening):
                continue
            name = names[step]
            prefix = f"[COOP_UI] {name} "
            for line in reversed(text.splitlines()):
                if not line.startswith(prefix):
                    continue
                x, y = map(float, line[len(prefix):].split())
                if not (0 <= x <= 800 and 0 <= y <= 600):
                    raise ValueError(f"invalid lobby hit region: {line}")
                with (root / "lobby.script").open("a", encoding="utf-8") as script:
                    script.write(f"click {x} {y}\nwait 30\n")
                    if name == "Ready":
                        script.write(f"shot {root / 'lobby.tga'}\n")
                self.actions.append(dict(slot=slot, control=name, x=x, y=y))
                self.steps[slot] += 1
                break


class ClientShim:
    """Datagram faults below ENet, including ACK loss and retransmission delay."""
    def __init__(self, loss: float, latency: float, jitter: float):
        self.loss, self.latency, self.jitter = loss, latency, jitter
        self.random = random.Random(12345)
        self.pending = []
        self.received = self.dropped = self.forwarded = 0
        self.stall_until = 0.0
        self.minimum, self.maximum = float("inf"), 0.0

    def enqueue(self, packet, destination, now):
        self.received += 1
        if self.random.random() * 100 < self.loss:
            self.dropped += 1
            return
        delay = self.latency + self.random.uniform(-self.jitter, self.jitter)
        self.minimum, self.maximum = min(self.minimum, delay), max(self.maximum, delay)
        heapq.heappush(self.pending, (now + delay / 1000, self.received, packet, destination))

    def ready(self, now):
        while self.pending and self.pending[0][0] <= now and now >= self.stall_until:
            _, _, packet, destination = heapq.heappop(self.pending)
            self.forwarded += 1
            yield packet, destination

    def receipt(self):
        return dict(received=self.received, dropped=self.dropped, forwarded=self.forwarded,
                    delay_min_ms=self.minimum if self.received > self.dropped else None,
                    delay_max_ms=self.maximum)


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", required=True, type=Path)
    ap.add_argument("--role", choices=("host", "join", "both"), default="both")
    ap.add_argument("--endpoint", help="numeric host IP:port; join connects directly in single-role mode")
    ap.add_argument("--compare-only", action="store_true", help="compare out/session/{host,client}/session.trace without launching")
    ap.add_argument("--client-install", type=Path, help="separate, privately writable client install")
    ap.add_argument("--address", default="127.0.0.1", help="numeric local LAN IPv4 address")
    ap.add_argument("--loss", type=float, default=0, help="datagram loss percent in each direction")
    ap.add_argument("--latency", type=float, default=0, help="one-way added latency in ms")
    ap.add_argument("--jitter", type=float, default=0, help="uniform +/- latency jitter in ms")
    ap.add_argument("--stall-ms", type=int, default=0)
    ap.add_argument("--stall-tick", type=int, default=300)
    ap.add_argument("--kill-peer", choices=("host", "client", "both"))
    ap.add_argument("--kill-tick", type=int, default=600)
    ap.add_argument("--combat", action="store_true")
    ap.add_argument("--lobby", action="store_true", help="drive the rendered 1080p loopback lobby widgets")
    ap.add_argument("--wall-clock", action="store_true", help="normal clock, at least 900 ticks, assert 30 Hz +/-5%%")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--ticks", type=int, default=18000)
    ap.add_argument("--port", type=int, default=23460)
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--render", choices=("none", "host", "client", "both"), default="none")
    ap.add_argument("--force-desync", type=int, default=0)
    return ap


def arguments(argv=None):
    ap = parser()
    args = ap.parse_args(argv)
    if args.endpoint:
        try:
            args.address, port = args.endpoint.rsplit(":", 1)
            args.port = int(port)
        except ValueError:
            ap.error("endpoint must be numeric IPv4:port")
    try:
        ip = ipaddress.IPv4Address(args.address)
        if ip.is_unspecified or ip.is_multicast or ip.is_link_local or int(ip) == 0xffffffff:
            raise ValueError()
    except ValueError:
        ap.error("address must be a numeric unicast IPv4 address")
    if (args.ticks < 64 or not 0 <= args.force_desync <= args.ticks or not 1 <= args.port < 65535
            or args.timeout <= 0 or not 0 <= args.loss < 100
            or not 0 <= args.jitter <= args.latency < float("inf") or args.stall_ms < 0
            or (args.stall_ms and not 1 <= args.stall_tick < args.ticks)
            or (args.kill_peer and not 1 <= args.kill_tick < args.ticks)
            or (args.force_desync and (args.kill_peer or args.combat))):
        ap.error("invalid tick, port, timeout or fault bounds (jitter<=latency, loss<100)")
    if args.wall_clock and (args.ticks < 900 or args.force_desync or args.kill_peer or args.stall_ms
                           or (not args.combat and (args.loss or args.latency))):
        ap.error("wall-clock requires >=900 ticks; network faults require a combat cell")
    args.install = args.install.resolve()
    args.client_install = (args.client_install or args.install).resolve()
    if args.lobby and (args.role != "both" or args.render != "both" or not ip.is_loopback or args.port != 23460):
        ap.error("lobby proof requires --role both --render both and loopback port 23460")
    if args.role != "both" and (args.loss or args.latency or args.stall_ms or args.kill_peer or args.wall_clock):
        ap.error("single-role mode requires direct transport without pair fault/pacing controls")
    if args.role == "both" and not args.compare_only and not ip.is_loopback and args.install == args.client_install:
        ap.error("LAN proof requires two isolated installs")
    return args


def run_role(args, out):
    slot = 0 if args.role == "host" else 1
    peer = out / ("host" if slot == 0 else "client")
    peer.mkdir(parents=True, exist_ok=False)
    (peer / "orders.txt").write_text("\n".join(fixture_orders(slot, args.ticks, args.force_desync, args.combat)) + "\n")
    proc = None
    try:
        with ExitStack() as logs:
            deploy_lease.acquire_lease(str(args.install), intended_test="mp LAN role proof")
            logs.callback(deploy_lease.release_lease, str(args.install))
            if args.render != "none":
                host_lease.acquire("mp LAN role proof", timeout_s=120)
                logs.callback(host_lease.release)
            proc = peer_start(str(args.install), str(peer), slot, args.port, args.ticks,
                              args.timeout, args.render != "none", logs, args.address, args.combat)
            rc = proc.wait(timeout=args.timeout)
            if rc != (3 if args.force_desync else 0):
                raise ValueError(f"role exited {rc}; see peer logs")
            if "[SHUTDOWN] audio end" not in (peer / "stderr.log").read_text(errors="replace"):
                raise ValueError("engine shutdown incomplete")
            result = dict(result="PASS", role=args.role, comparison="PENDING --compare-only")
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        result = dict(result="FAIL", error=str(error))
    finally:
        if proc is not None:
            cleanup_kill = proc.poll() is None
            if cleanup_kill:
                proc.kill()
            proc.wait()
            (peer / "exit.json").write_text(json.dumps(dict(rc=proc.returncode, pid=proc.pid, cleanup_kill=cleanup_kill)))
    return result


def run_pair(args, out, kill_peer=None):
    out.mkdir(parents=True, exist_ok=False)
    peers = [out / "host", out / "client"]
    installs = [args.install, args.client_install]
    for slot, directory in enumerate(peers):
        directory.mkdir()
        script = fixture_orders(slot, args.ticks, args.force_desync, args.combat)
        (directory / "orders.txt").write_text("\n".join(script) + "\n", encoding="utf-8")
    processes = []
    started = time.monotonic()
    shim = ClientShim(args.loss, args.latency, args.jitter)
    events, ticks = [], [0, 0]
    progress = [[], []]
    lobby_driver = LobbyDriver(peers) if args.lobby else None
    killed = None
    try:
        with ExitStack() as logs:
            if args.render != "none":
                host_lease.acquire("mp LAN proof", timeout_s=120)
            for install in dict.fromkeys(installs):
                deploy_lease.acquire_lease(str(install), intended_test="mp LAN proof")
                logs.callback(deploy_lease.release_lease, str(install))
            relay = logs.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            relay.bind((args.address, args.port + 1))
            upstream, client = (args.address, args.port), None
            for slot, directory in enumerate(peers):
                rendered = args.render == "both" or args.render == ("host" if slot == 0 else "client")
                processes.append(peer_start(str(installs[slot]), str(directory), slot, args.port + (0 if args.lobby else slot),
                    args.ticks, args.timeout, rendered, logs, args.address, args.combat, args.wall_clock, args.lobby))
            offsets = [0, 0]
            while any(proc.poll() is None for proc in processes):
                now = time.monotonic()
                if lobby_driver:
                    lobby_driver.pump()
                if now - started > args.timeout:
                    raise ValueError("pair exceeded proof timeout")
                for slot, directory in enumerate(peers):
                    path = directory / "session.trace"
                    if path.exists():
                        with path.open(encoding="utf-8") as trace:
                            trace.seek(offsets[slot])
                            while line := trace.readline():
                                if not line.endswith("\n"):
                                    break
                                offsets[slot] = trace.tell()
                                if line.startswith("H "):
                                    ticks[slot] = int(line.split()[1])
                                    sample = (ticks[slot], now)
                                    if not progress[slot]:
                                        progress[slot] = [sample, sample]
                                    progress[slot][-1] = sample
                if args.stall_ms and not events and min(ticks) >= args.stall_tick:
                    shim.stall_until = now + args.stall_ms / 1000
                    events.append(dict(event="stall", ticks=ticks.copy(), wall=now-started, ms=args.stall_ms))
                if events and events[-1]["event"] == "stall" and now >= shim.stall_until:
                    events.append(dict(event="resume", ticks=ticks.copy(), wall=now-started))
                if kill_peer and killed is None and min(ticks) >= args.kill_tick:
                    killed = 0 if kill_peer == "host" else 1
                    processes[killed].kill()
                    events.append(dict(event="kill", peer=kill_peer, ticks=ticks.copy(), wall=now-started))
                for packet, destination in shim.ready(now):
                    relay.sendto(packet, destination)
                if select.select([relay], [], [], 0.005)[0]:
                    try:
                        packet, source = relay.recvfrom(65535)
                    except ConnectionResetError:
                        # Windows reports UDP ICMP after a killed peer; ENet owns the timeout.
                        continue
                    if source == upstream:
                        if client is not None:
                            shim.enqueue(packet, client, time.monotonic())
                    else:
                        if client is not None and source != client:
                            raise ValueError("unexpected third peer at client shim")
                        client = source
                        shim.enqueue(packet, upstream, time.monotonic())
            for directory, proc in zip(peers, processes):
                proc.wait()
                (directory / "exit.json").write_text(json.dumps({"rc": proc.returncode, "pid": proc.pid}), encoding="utf-8")
            if lobby_driver:
                (out / "menu-flow.json").write_text(json.dumps(lobby_driver.actions, indent=2), encoding="utf-8")
                if lobby_driver.steps != [4, 3]:
                    raise ValueError(f"incomplete menu flow: {lobby_driver.steps}")
            if kill_peer:
                if killed is None:
                    raise ValueError("kill tick was never reached")
                survivor = peers[1-killed]
                text = (survivor / "session.trace").read_text(encoding="utf-8")
                if processes[1-killed].returncode != 3 or not any(message in text for message in (
                        "STOP MP peer disconnected", "STOP MP peer stalled")):
                    raise ValueError("survivor did not stop cleanly with a peer-loss message")
                a, _ = read_trace(peers[0] / "session.trace")
                b, _ = read_trace(peers[1] / "session.trace")
                common = min(len(a), len(b))
                if "[MP_DESYNC]" in text or common < args.kill_tick or any(a[t] != b[t] for t in range(1, common + 1)):
                    raise ValueError("kill proof has incomplete or divergent pre-kill hashes")
                result = dict(equal_ticks=common, stop_message=[line for line in text.splitlines() if line.startswith("STOP ")])
                elapsed = time.monotonic() - started - next(e["wall"] for e in events if e["event"] == "kill")
                if elapsed > 20:
                    raise ValueError(f"survivor shutdown exceeded 20 seconds: {elapsed:.2f}")
                result["peer_loss_shutdown_seconds"] = round(elapsed, 3)
            else:
                if any(proc.returncode not in ((0, 3) if args.wall_clock else (3 if args.force_desync else 0,)) for proc in processes):
                    raise ValueError("a peer failed; see peer launcher/stderr logs")
                compared = args.ticks
                if args.wall_clock:
                    compared = min(len(read_trace(peer / "session.trace")[0]) for peer in peers)
                    if not args.combat and compared < args.ticks:
                        raise ValueError("wall-clock run did not reach the required tick count")
                result = compare_traces(*peers, compared, args.force_desync, args.combat, 12 if args.wall_clock else 2)
                if args.wall_clock:
                    rates = [(last[0] - first[0]) / (last[1] - first[1]) if last[1] > first[1] else 0
                             for first, last in progress]
                    coverage = [last[1] - first[1] for first, last in progress]
                    if args.combat and min(coverage) < args.ticks / 30 - 0.5:
                        raise ValueError(f"combat did not cover the requested wall duration: {coverage}")
                    if not args.combat and not all(28.5 <= rate <= 31.5 for rate in rates):
                        raise ValueError(f"wall-clock pacing outside 30 Hz +/-5%: {rates}")
                    result["ticks_per_wall_second"] = rates
                    result["trace_wall_seconds"] = coverage
                if args.stall_ms and (not any(e["event"] == "resume" for e in events) or min(ticks) <= args.stall_tick + 30):
                    raise ValueError("stall was not injected and recovered")
            for slot, directory in enumerate(peers):
                if slot != killed and "[SHUTDOWN] audio end" not in (directory / "stderr.log").read_text(encoding="utf-8", errors="replace"):
                    raise ValueError(f"engine shutdown incomplete in {directory}")
                if args.wall_clock and processes[slot].returncode == 3:
                    text = (directory / "session.trace").read_text(encoding="utf-8")
                    if not any(message in text for message in ("STOP MP peer disconnected", "STOP MP peer stalled")):
                        raise ValueError("unexpected stop during wall-clock proof")
            if args.loss and shim.dropped == 0:
                raise ValueError("loss injection did not drop a datagram")
            if args.render != "none" and not any(out.rglob("*.png")):
                raise ValueError("requested off-screen capture is missing")
            result.update(result="PASS", graceful_shutdown=True)
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        result = {"result": "FAIL", "error": str(error)}
    finally:
        for directory, proc in zip(peers, processes):
            cleanup_kill = proc.poll() is None
            if cleanup_kill:
                proc.kill()
            proc.wait()
            (directory / "exit.json").write_text(json.dumps(dict(rc=proc.returncode, pid=proc.pid,
                cleanup_kill=cleanup_kill)), encoding="utf-8")
        host_lease.release()
    result.update(peer_exit_codes=[proc.returncode for proc in processes], ticks=ticks,
                  wall_seconds=round(time.monotonic() - started, 2), address=args.address,
                  installs=[str(path) for path in installs], faults=shim.receipt(), events=events)
    result["traces"] = {}
    for directory in peers:
        path = directory / "session.trace"
        if path.exists():
            text = path.read_text(encoding="utf-8", errors="replace")
            result["traces"][directory.name] = dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                network=[line for line in text.splitlines() if line.startswith(("IDENTITY ", "NETWORK ", "STOP ", "[MP_DESYNC]"))])
    (out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def main() -> int:
    args = arguments()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=args.compare_only or args.role != "both")
    directions = ["client", "host"] if args.kill_peer == "both" else [args.kill_peer]
    if args.compare_only:
        try:
            runs = [dict(result="PASS", **compare_traces(args.out / "session/host", args.out / "session/client",
                         args.ticks, args.force_desync, args.combat))]
        except (ValueError, OSError) as error:
            runs = [dict(result="FAIL", error=str(error))]
    elif args.role != "both":
        runs = [run_role(args, args.out / "session")]
    else:
        runs = [run_pair(args, args.out / (direction or "session"), direction) for direction in directions]
    result = dict(result="PASS" if all(run["result"] == "PASS" for run in runs) else "FAIL", runs=runs)
    (args.out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    images = []
    for png in args.out.rglob("*.png"):
        images.append(f'<h2>{png.relative_to(args.out)}</h2><img style="max-width:100%" alt="{png.parent.name}" '
                      f'src="data:image/png;base64,{base64.b64encode(png.read_bytes()).decode()}">')
    (args.out / "RESULT.html").write_text('<!doctype html><meta charset="utf-8"><title>MC2 co-op proof</title>'
        + '<pre>' + html.escape(json.dumps(result, indent=2)) + '</pre>' + ''.join(images)
        + ("<p>Off-screen screenshots NOT RUN (headless protocol/combat proof).</p>" if not images else ""), encoding="utf-8")
    print(json.dumps(result))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Linux counterpart of run_mp_loopback_proof.py; fold into it after mp-4 merges.

Run only under slot.sh N --game. Imports its read_trace/compare_traces (lines
28-75) and fixture_orders (78-93) unchanged; peer_command mirrors peer_start
(96-130), replacing the Windows executable/spawner. ClientShim and argument
validation are imported unchanged from the Windows runner (132-220).
"""
from __future__ import annotations

from contextlib import ExitStack
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import time

from run_mp_loopback_proof import ClientShim, arguments, compare_traces, fixture_orders, read_trace
from smoke_lib.runner import FORCED_ENV, SOFT_ENV, POPPED_ENV, unregistered_ambient


def peer_command(args, root, slot):
    env = os.environ.copy()
    for key in unregistered_ambient(env):
        env.pop(key, None)
    env.update(FORCED_ENV)
    for key, value in SOFT_ENV.items():
        env.setdefault(key, value)
    for key in (*POPPED_ENV, "MC2_COMMAND_REPLAY_PLAY", "MC2_COMMAND_REPLAY_RECORD"):
        env.pop(key, None)
    env.update(MC2_SMOKE_FIXED_TIMESTEP="1", MC2_SIM_IDENTITY_TRACE="1",
               MC2_FORCE_ALL_VISIBLE="0", MC2_DETERMINISTIC_RNG="1", MC2_RNG_SEED="12345",
               MC2_DIAGNOSTIC_TRACE_FILE=str(root / "diagnostic.jsonl"),
               MC2_SAVE_DIR=str(root / "saves"),
               MC2_CAMERA_PARK=("2450,-2100,0,60,1200@30" if slot == 0
                                else "2450,-2100,90,50,1200@30"))
    if args.combat:
        env["MC2_CAMERA_PARK"] = "3000,1900,0,60,1700@30"
    argv = [str(args.install / "mc2"), "--mission", "mc2_01", "--profile", "tier1",
            "--duration", str(args.timeout), "--ticks", str(args.ticks),
            "--host" if slot == 0 else "--join", f"127.0.0.1:{args.port + slot}",
            "--mp-user-dir", str(root), "--headless"]
    return argv, env


def text(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def evaluate(peers, ticks, codes, combat=False, expected_codes=None, max_extra=2):
    result = dict(result="FAIL", ticks_reached=[], peer_exit_codes=codes,
                  first_divergent_tick=None, first_divergent_field=None,
                  equal_ticks=0, handshake=[], desync_lines=[], first_failing_line=None)
    hashes = []
    errors = []
    logs = []
    for peer in peers:
        trace = text(peer / "session.trace")
        combined = "\n".join(text(peer / name) for name in
                             ("launcher.log", "stdout.log", "stderr.log", "diagnostic.jsonl", "session.trace"))
        logs.append(combined)
        result["handshake"].append([line for line in trace.splitlines()
                                    if line.startswith(("IDENTITY ", "NETWORK "))])
        result["desync_lines"].extend(line for line in combined.splitlines() if "[MP_DESYNC]" in line)
        try:
            rows, _ = read_trace(peer / "session.trace")
        except (OSError, ValueError) as exc:
            rows = {}
            errors.append(str(exc))
        hashes.append(rows)
        result["ticks_reached"].append(len(rows))
        refusal = next((line for line in combined.splitlines() if any(marker in line.lower()
                        for marker in ("sha-256 mismatch", "handshake refused", "protocol version mismatch"))), None)
        if refusal:
            errors.append(refusal)
            result["classification"] = "handshake refusal"
        if not any(line.startswith("NETWORK ") for line in trace.splitlines()):
            errors.append(f"{peer.name}: no NETWORK record (peer never connects)")
    for tick in range(1, min(map(len, hashes)) + 1):
        if hashes[0][tick] != hashes[1][tick]:
            result["first_divergent_tick"] = tick
            index = next(i for i, pair in enumerate(zip(hashes[0][tick], hashes[1][tick])) if pair[0] != pair[1])
            result["first_divergent_field"] = f"H[{index + 2}]"
            result["divergent_values"] = [rows[tick][index] for rows in hashes]
            break
        result["equal_ticks"] += 1
    result["simid_detail"] = [[line for line in log.splitlines() if "[SIMID]" in line][-20:] for log in logs]
    if codes != (expected_codes if expected_codes is not None else [0, 0]):
        errors.append(f"peer exit codes: {codes}")
    if result["desync_lines"]:
        errors.append(result["desync_lines"][0])
        result["classification"] = "desync"
    try:
        result["comparison"] = compare_traces(*peers, ticks, combat=combat, max_extra=max_extra)
    except (OSError, ValueError) as exc:
        errors.append(str(exc))
    if result["first_divergent_tick"] is not None:
        errors.insert(0, f"first divergent tick={result['first_divergent_tick']} field={result['first_divergent_field']}")
        result["classification"] = "desync"
    result["errors"] = errors
    result["first_failing_line"] = errors[0] if errors else None
    result.setdefault("classification", "peer never connects" if any("no NETWORK" in e for e in errors)
                      else "crash" if any(code is not None and code < 0 for code in codes)
                      else "headless or path defect" if errors else "connected")
    result["result"] = "FAIL" if errors else "PASS"
    return result


def peak_rss(pid):
    # VmHWM is the kernel high-water mark, sampled while each owned PID exists.
    try:
        status = text(Path(f"/proc/{pid}/status"))
    except OSError:  # The peer may exit between poll() and the /proc read.
        return 0
    for line in status.splitlines():
        if line.startswith("VmHWM:"):
            return int(line.split()[1])
    return 0


def stop(processes):
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
    for proc in processes:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def run_pair(args):
    args.install = args.install.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    peers = [args.out / "session" / name for name in ("host", "client")]
    for slot, peer in enumerate(peers):
        peer.mkdir(parents=True)
        (peer / "saves").mkdir()
        (peer / "orders.txt").write_text("\n".join(fixture_orders(slot, args.ticks, 0, args.combat)) + "\n", encoding="utf-8")
    processes, peak, network, offsets = [], [0, 0], [None, None], [0, 0]
    started = time.monotonic()
    failure = None
    shim = ClientShim(args.loss, args.latency, args.jitter)
    events, ticks, exits, cleanup = [], [0, 0], [None, None], [False, False]
    killed = None
    try:
        with ExitStack() as logs:
            relay = logs.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            relay.bind(("127.0.0.1", args.port + 1))
            upstream, client = ("127.0.0.1", args.port), None
            for slot, peer in enumerate(peers):
                argv, env = peer_command(args, peer, slot)
                log = logs.enter_context((peer / "launcher.log").open("w", encoding="utf-8"))
                # background-launch: allow Linux headless peers under slot.sh --game; only owned PIDs are stopped
                processes.append(subprocess.Popen(argv, cwd=args.install, env=env, stdout=log, stderr=subprocess.STDOUT))
            while any(proc.poll() is None for proc in processes):
                now = time.monotonic()
                elapsed = now - started
                if elapsed > args.timeout:
                    raise TimeoutError("pair exceeded proof timeout")
                for slot, (peer, proc) in enumerate(zip(peers, processes)):
                    peak[slot] = max(peak[slot], peak_rss(proc.pid))
                    path = peer / "session.trace"
                    if path.exists():
                        with path.open(encoding="utf-8", errors="replace") as trace:
                            trace.seek(offsets[slot])
                            while line := trace.readline():
                                if not line.endswith("\n"):
                                    break
                                offsets[slot] = trace.tell()
                                if line.startswith("NETWORK ") and network[slot] is None:
                                    network[slot] = elapsed
                                if line.startswith("H "):
                                    ticks[slot] = int(line.split()[1])
                    if proc.poll() is not None and exits[slot] is None:
                        exits[slot] = elapsed
                        # A fault victim must not cause us to terminate its survivor.
                        if proc.returncode != 0 and not (args.kill_peer or args.stall_ms):
                            raise RuntimeError(f"{peer.name} exited {proc.returncode}")
                        if ticks[slot] < args.ticks and not (args.kill_peer or args.stall_ms):
                            raise RuntimeError(f"{peer.name} exited before tick target")
                if args.stall_ms and not events and min(ticks) >= args.stall_tick:
                    shim.stall_until = now + args.stall_ms / 1000
                    events.append(dict(event="stall", ticks=ticks.copy(), wall=elapsed, ms=args.stall_ms))
                if events and events[-1]["event"] == "stall" and now >= shim.stall_until:
                    events.append(dict(event="resume", ticks=ticks.copy(), wall=elapsed))
                if args.kill_peer and killed is None and min(ticks) >= args.kill_tick:
                    killed = 0 if args.kill_peer == "host" else 1
                    if processes[killed].poll() is not None:
                        raise RuntimeError("kill target exited before injection")
                    processes[killed].kill()
                    events.append(dict(event="kill", peer=args.kill_peer, pid=processes[killed].pid,
                                       ticks=ticks.copy(), wall=elapsed))
                injection = next((e for e in events if e["event"] in ("kill", "stall")), None)
                if injection and elapsed - injection["wall"] > 20 and (args.kill_peer or any(p.returncode == 3 for p in processes)):
                    raise TimeoutError("fault shutdown exceeded 20 seconds")
                for packet, destination in shim.ready(now):
                    relay.sendto(packet, destination)
                if select.select([relay], [], [], 0.005)[0]:
                    packet, source = relay.recvfrom(65535)
                    if source == upstream:
                        if client is not None:
                            shim.enqueue(packet, client, time.monotonic())
                    else:
                        if client is not None and client != source:
                            raise ValueError("unexpected third peer at client shim")
                        client = source
                        shim.enqueue(packet, upstream, time.monotonic())
    except (OSError, ValueError, RuntimeError, TimeoutError, KeyboardInterrupt) as exc:
        failure = f"{type(exc).__name__}: {exc}"
    finally:
        cleanup = [proc.poll() is None for proc in processes]
        stop(processes)
    codes = [proc.returncode for proc in processes]
    codes.extend([None] * (2 - len(codes)))
    stopped_stall = args.stall_ms and codes == [3, 3]
    expected = codes if killed is not None or stopped_stall else None
    compared = min(ticks) if expected else args.ticks
    result = evaluate(peers, compared, codes, args.combat, expected, args.ticks if expected else 2)
    errors = result["errors"]
    if args.kill_peer:
        survivor = 1 - killed if killed is not None else None
        if survivor is None or codes[survivor] != 3 or cleanup[survivor]:
            errors.append("survivor did not exit independently with code 3 after kill injection")
        elif not any(marker in text(peers[survivor] / "session.trace") for marker in
                     ("STOP MP peer disconnected", "STOP MP peer stalled")):
            errors.append("survivor missing peer-loss diagnostic")
        if min(ticks) < args.kill_tick:
            errors.append("incomplete pre-kill hashes")
    if args.stall_ms:
        if not any(e["event"] == "stall" for e in events):
            errors.append("stall was not injected")
        if stopped_stall and any("STOP MP peer stalled" not in text(p / "session.trace") for p in peers):
            errors.append("both stalled peers must report STOP MP peer stalled")
        if not stopped_stall and not any(e["event"] == "resume" for e in events):
            errors.append("stall did not resume")
    if expected:
        injection = next(e for e in events if e["event"] in ("kill", "stall")) if events else None
        shutdown = max((value or time.monotonic() - started) for value in exits) - injection["wall"] if injection else None
        result["peer_loss_shutdown_seconds"] = shutdown
        if shutdown is None or shutdown > 20 or any(cleanup):
            errors.append("fault shutdown was not independent within 20 seconds")
    if args.loss and not shim.dropped:
        errors.append("loss injection did not drop a datagram")
    result.update(result="FAIL" if errors else "PASS", first_failing_line=errors[0] if errors else None)
    if failure:
        result.update(result="FAIL", runner_error=failure)
        result["first_failing_line"] = result["first_failing_line"] or failure
    result.update(wall_seconds=time.monotonic() - started, peak_rss_kib=peak,
                  rendezvous_seconds=max(network) if all(n is not None for n in network) else None,
                  pids=[proc.pid for proc in processes], requested_ticks=args.ticks, combat=args.combat)
    result.update(faults=shim.receipt(), events=events, cleanup_kill=cleanup, exit_wall_seconds=exits)
    (args.out / "verdict.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{result['result']} ticks={result['ticks_reached']} equal={result['equal_ticks']} "
          f"wall={result['wall_seconds']:.2f}s verdict={args.out / 'verdict.json'}", flush=True)
    return result


def main(argv=None):
    args = arguments(argv)
    if (args.role != "both" or args.address != "127.0.0.1" or args.compare_only
            or args.render != "none" or args.wall_clock or args.force_desync
            or args.client_install != args.install or args.kill_peer == "both"):
        raise SystemExit("Linux pair supports one headless loopback pair per invocation")
    return 0 if run_pair(args)["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

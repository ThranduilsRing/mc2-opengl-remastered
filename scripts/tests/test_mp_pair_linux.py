"""Offline Linux-pair contract and lifecycle proofs; no game executable needed."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_mp_pair_linux as pair

POPEN_CLASS = subprocess.Popen

def options(tmp_path, **changes):
    values = dict(install=tmp_path, out=tmp_path / "proof", ticks=64, port=23460, timeout=5, combat=False,
                  loss=0, latency=0, jitter=0, stall_ms=0, stall_tick=30, kill_peer=None, kill_tick=30)
    values.update(changes)
    return argparse.Namespace(**values)


def trace(peer, ticks=64):
    peer.mkdir(parents=True, exist_ok=True)
    (peer / "session.trace").write_text("IDENTITY executable=abc content=def\nNETWORK rtt_ms=0\nC 1 00\n" +
                                      "".join(f"H {t} a b c d {t:x}\n" for t in range(1, ticks + 1)), encoding="utf-8")


@pytest.mark.parametrize("slot", [0, 1], ids=["host", "join"])
@pytest.mark.parametrize("combat", [False, True], ids=["movement", "combat"])
def test_argv_environment_and_fixture_per_role(tmp_path, monkeypatch, slot, combat):
    monkeypatch.setenv("MC2_COMMAND_REPLAY_PLAY", "ambient")
    monkeypatch.setenv("MC2_COMMAND_REPLAY_RECORD", "ambient")
    args = options(tmp_path, combat=combat)
    peer = tmp_path / str(slot)
    argv, env = pair.peer_command(args, peer, slot)
    assert argv == [str(tmp_path / "mc2"), "--mission", "mc2_01", "--profile", "tier1",
                    "--duration", "5", "--ticks", "64", "--host" if slot == 0 else "--join",
                    f"127.0.0.1:{23460 + slot}", "--mp-user-dir", str(peer), "--headless"]
    assert all(env[key] == value for key, value in dict(MC2_SMOKE_FIXED_TIMESTEP="1",
               MC2_SIM_IDENTITY_TRACE="1", MC2_FORCE_ALL_VISIBLE="0", MC2_DETERMINISTIC_RNG="1", MC2_RNG_SEED="12345").items())
    assert env["MC2_DIAGNOSTIC_TRACE_FILE"] == str(peer / "diagnostic.jsonl")
    assert env["MC2_SAVE_DIR"] == str(peer / "saves")
    assert env["MC2_CAMERA_PARK"] == ("3000,1900,0,60,1700@30" if combat else
                "2450,-2100,0,60,1200@30" if slot == 0 else "2450,-2100,90,50,1200@30")
    assert not any(key in env for key in ("MC2_COMMAND_REPLAY_PLAY", "MC2_COMMAND_REPLAY_RECORD"))


@pytest.mark.parametrize("case", ["equal", "divergent", "desync", "refusal", "content-refusal", "missing-network", "short", "commands", "exit", "truncated-command", "truncated-combat", "gapped-tick", "unequal-row"])
def test_verdict_synthetic_traces(tmp_path, case):
    peers = [tmp_path / "host", tmp_path / "client"]
    for peer in peers:
        trace(peer)
    path = peers[1] / "session.trace"
    content = path.read_text()
    codes = [0, 0]
    if case == "divergent":
        content = content.replace("H 17 a b", "H 17 z b")
    elif case == "desync":
        (peers[0] / "stderr.log").write_text("[MP_DESYNC] tick=17 local=1 remote=2\n")
    elif case == "refusal":
        (peers[0] / "stderr.log").write_text("executable SHA-256 mismatch local=abc remote=xyz\n")
    elif case == "content-refusal":
        (peers[0] / "stderr.log").write_text("content SHA-256 mismatch local=def remote=xyz\n")
    elif case == "missing-network":
        content = content.replace("NETWORK rtt_ms=0\n", "")
    elif case == "short":
        content = content.replace("H 64 a b c d 40\n", "")
    elif case == "commands":
        content = content.replace("C 1 00", "C 1 ff")
    elif case == "exit":
        codes[1] = 1
    elif case == "truncated-command":
        content = content.replace("C 1 00", "C")
    elif case == "truncated-combat":
        content += "B 3 0\n"
    elif case == "gapped-tick":
        content = content.replace("H 17 a b c d 11\n", "")
    elif case == "unequal-row":
        content = content.replace("H 17 a b c d 11", "H 17 a b c d")
    path.write_text(content)
    verdict = pair.evaluate(peers, 64, codes, **({"combat": True} if case == "truncated-combat" else {}))
    assert verdict["result"] == ("PASS" if case == "equal" else "FAIL")
    if case == "divergent":
        assert verdict["first_divergent_tick"] == 17
        assert verdict["first_divergent_field"] == "H[2]"
        assert verdict["equal_ticks"] == 16


@pytest.mark.parametrize("case", ["gapped-tick", "unequal-row"])
def test_evaluate_irregular_hash_rows(tmp_path, monkeypatch, case):
    peers = [tmp_path / "host", tmp_path / "client"]
    for peer in peers:
        trace(peer)
    original = pair.read_trace

    def irregular(path):
        rows, orders = original(path)
        if path.parent == peers[1]:
            if case == "gapped-tick":
                del rows[17]
            else:
                rows[17] = rows[17][:-1]
        return rows, orders

    monkeypatch.setattr(pair, "read_trace", irregular)
    verdict = pair.evaluate(peers, 64, [0, 0])
    assert verdict["result"] == "FAIL"
    assert verdict["first_divergent_tick"] == 17
    if case == "unequal-row":
        assert verdict["divergent_values"] == ["11", "<missing>"]


@pytest.fixture
def fake_peer(tmp_path, monkeypatch):
    monkeypatch.setattr(pair.sys, "platform", "linux")
    stub = tmp_path / "fake_peer.py"
    stub.write_text('''import argparse, os, pathlib, time
p = argparse.ArgumentParser()
p.add_argument('--mp-user-dir')
p.add_argument('--ticks', type=int)
a, unknown = p.parse_known_args()
root = pathlib.Path(a.mp_user_dir)
print('launcher stdout', flush=True)
import sys
print('launcher stderr', file=sys.stderr, flush=True)
if os.environ.get('PAIR_FAKE_RELAY'):
    import socket
    host = '--host' in unknown
    address = unknown[unknown.index('--host' if host else '--join') + 1]
    endpoint = ('127.0.0.1', int(address.rsplit(':', 1)[1]))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
        relay.settimeout(2)
        if host:
            relay.bind(endpoint)
            packet, client = relay.recvfrom(100)
            relay.sendto(packet, client)
        else:
            time.sleep(0.1)
            relay.sendto(b'fault proof', endpoint)
            assert relay.recvfrom(100)[0] == b'fault proof'
assert (root / 'orders.txt').is_file()
assert os.environ['MC2_SAVE_DIR'] == str(root / 'saves')
assert os.environ['MC2_DIAGNOSTIC_TRACE_FILE'] == str(root / 'diagnostic.jsonl')
(root / 'stdout.log').write_text('fake stdout')
(root / 'stderr.log').write_text('fake stderr')
(root / 'diagnostic.jsonl').write_text('{}\\n')
(root / 'session.trace').write_text('IDENTITY executable=abc content=def\\nNETWORK rtt_ms=0\\nC 1 00\\n' + ''.join(f'H {t} a b c d {t:x}\\n' for t in range(1, a.ticks + 1 - int(os.environ.get('PAIR_FAKE_SHORT') == root.name))))
if os.environ.get('PAIR_FAKE_DIE') == root.name:
    raise SystemExit(2)
if os.environ.get('PAIR_FAKE_HANG') in ('1', root.name):
    time.sleep(60)
''', encoding="utf-8")
    real_spawn = subprocess.Popen
    children = []

    def spawn(argv, **kwargs):
        proc = real_spawn([sys.executable, str(stub), *argv[1:]], **kwargs)
        children.append(proc)
        return proc

    monkeypatch.setattr(pair.subprocess, "Popen", spawn)
    return children


@pytest.mark.parametrize("combat", [False, True], ids=["movement", "combat"])
def test_fake_pair_fixture_creation_and_directory_isolation(tmp_path, fake_peer, combat):
    args = options(tmp_path, combat=combat)
    verdict = pair.run_pair(args)
    # Combat comparison itself is covered by the imported Windows proof; stub has no battle events.
    assert verdict["result"] == ("FAIL" if combat else "PASS")
    assert verdict["peer_exit_codes"] == [0, 0]
    for slot, name in enumerate(("host", "client")):
        root = args.out / "session" / name
        assert (root / "orders.txt").read_text().splitlines() == pair.fixture_orders(slot, 64, 0, combat)
        assert all((root / filename).exists() for filename in
                   ("stdout.log", "stderr.log", "diagnostic.jsonl", "session.trace", "launcher.log", "saves"))
    assert len(fake_peer) == 2 and all(proc.poll() == 0 for proc in fake_peer)
    assert json.loads((args.out / "verdict.json").read_text())["ticks_reached"] == [64, 64]


@pytest.mark.parametrize("failure", ["timeout", "peer-dies", "short-peer", "client-dies", "short-client", "interrupt", "second-spawn"])
def test_owned_children_reaped_on_every_exit(tmp_path, monkeypatch, fake_peer, failure):
    monkeypatch.setenv("PAIR_FAKE_HANG", "1")
    args = options(tmp_path, timeout=0.4)
    if failure in ("peer-dies", "short-peer"):
        monkeypatch.setenv("PAIR_FAKE_HANG", "client")
        monkeypatch.setenv("PAIR_FAKE_DIE" if failure == "peer-dies" else "PAIR_FAKE_SHORT", "host")
    elif failure in ("client-dies", "short-client"):
        args.timeout = 5
        monkeypatch.setenv("PAIR_FAKE_HANG", "host")
        monkeypatch.setenv("PAIR_FAKE_DIE" if failure == "client-dies" else "PAIR_FAKE_SHORT", "client")
    elif failure in ("interrupt", "second-spawn"):
        spawn = pair.subprocess.Popen

        def failing_spawn(argv, **kwargs):
            if fake_peer:
                if failure == "interrupt":
                    raise KeyboardInterrupt()
                raise OSError("synthetic second spawn failure")
            return spawn(argv, **kwargs)

        monkeypatch.setattr(pair.subprocess, "Popen", failing_spawn)
    verdict = pair.run_pair(args)
    assert verdict["result"] == "FAIL"
    assert set(verdict["peer_stdio"]) == {"host", "client"}
    if failure in ("client-dies", "short-client"):
        assert verdict["wall_seconds"] < 2
        assert verdict["peer_stdio"]["client"] == {
            "stdout": "fake stdoutlauncher stdout\n", "stderr": "fake stderrlauncher stderr\n"}
    assert fake_peer and all(proc.poll() is not None for proc in fake_peer)
    assert (args.out / "verdict.json").exists()
    if failure == "timeout":
        assert len(fake_peer) == 2
        assert all(proc.returncode != 0 for proc in fake_peer)
    if failure in ("peer-dies", "short-peer"):
        assert fake_peer[1].returncode != 0


def test_platform_guard_before_output_creation(tmp_path, monkeypatch):
    monkeypatch.setattr(pair.sys, "platform", "win32")
    with pytest.raises(RuntimeError, match="Linux only"):
        pair.run_pair(options(tmp_path))
    assert not (tmp_path / "proof").exists()


def test_module_description():
    assert pair.__doc__ == "Linux headless lockstep pair runner; run under slot.sh N --game."


def test_stop_reaps_all_owned_processes_after_second_interrupt():
    class Owned:
        returncode = None
        waited = False
        def poll(self):
            return self.returncode
        def terminate(self):
            pass
        def kill(self):
            self.returncode = -9
        def wait(self, timeout=None):
            if timeout is not None:
                raise KeyboardInterrupt()
            self.waited = True
    children = [Owned(), Owned()]
    with pytest.raises(KeyboardInterrupt):
        pair.stop(children)
    assert all(p.returncode == -9 and p.waited for p in children)


@pytest.mark.parametrize("fault", ["--loss=100", "--loss=-1", "--latency=-1", "--latency=inf",
    "--loss=nan", "--jitter=1", "--stall-ms=-1", "--stall-ms=1 --stall-tick=0",
    "--stall-ms=1 --stall-tick=64", "--kill-peer=client --kill-tick=0",
    "--kill-peer=host --kill-tick=64", "--port=65535"])
def test_shared_fault_bounds(tmp_path, fault):
    with pytest.raises(SystemExit):
        pair.main(["--install", str(tmp_path), "--out", str(tmp_path / "out"), "--ticks", "64", *fault.split()])


def test_shared_validation_and_fault_forwarding(tmp_path, monkeypatch):
    import run_mp_loopback_proof as windows
    assert pair.arguments is windows.arguments and pair.ClientShim is windows.ClientShim
    captured = []
    monkeypatch.setattr(pair, "run_pair", lambda args: captured.append(args) or {"result": "PASS"})
    assert pair.main(["--install", str(tmp_path), "--out", str(tmp_path / "out"), "--ticks", "3000",
        "--loss", "2", "--latency", "60", "--jitter", "20", "--stall-ms", "2000",
        "--stall-tick", "1500", "--kill-peer", "host", "--kill-tick", "1500"]) == 0
    args = captured[0]
    assert (args.loss, args.latency, args.jitter, args.stall_ms, args.stall_tick,
            args.kill_peer, args.kill_tick) == (2, 60, 20, 2000, 1500, "host", 1500)


@pytest.mark.skipif(os.name == "nt", reason="Linux UDP relay integration runs on the box")
def test_client_relay_forwards_both_directions(tmp_path, monkeypatch, fake_peer):
    monkeypatch.setenv("PAIR_FAKE_RELAY", "1")
    verdict = pair.run_pair(options(tmp_path, latency=10, jitter=2))
    assert verdict["result"] == "PASS", verdict
    assert verdict["faults"]["forwarded"] == 2
    assert 8 <= verdict["faults"]["delay_min_ms"] <= verdict["faults"]["delay_max_ms"] <= 12


@pytest.mark.parametrize("victim", ["host", "client"])
def test_kill_waits_for_survivor_independent_shutdown(tmp_path, monkeypatch, fake_peer, victim):
    monkeypatch.setenv("PAIR_FAKE_HANG", "1")
    real_poll = POPEN_CLASS.poll
    detected = []
    injected = []
    real_kill = POPEN_CLASS.kill

    def kill(proc):
        injected.append(proc.pid)
        return real_kill(proc)

    def poll(proc):
        rc = real_poll(proc)
        if len(fake_peer) == 2 and injected and proc.pid != injected[0] and rc is None:
            detected.append(proc.pid)
            # Simulate an engine observing the peer loss after several relay cycles.
            if len(detected) == 5:
                real_kill(proc)
                proc.wait()
                proc.returncode = 3
                peer = tmp_path / "proof" / "session" / ("client" if victim == "host" else "host")
                with (peer / "session.trace").open("a") as log:
                    log.write("STOP MP peer disconnected\n")
                rc = 3
        return rc

    monkeypatch.setattr(POPEN_CLASS, "kill", kill)
    monkeypatch.setattr(POPEN_CLASS, "poll", poll)
    verdict = pair.run_pair(options(tmp_path, kill_peer=victim))
    assert verdict["result"] == "PASS", verdict
    assert len(injected) == 1 and len(detected) >= 5
    assert verdict["cleanup_kill"] == [False, False]
    assert verdict["events"][0]["event"] == "kill"
    assert verdict["peer_loss_shutdown_seconds"] < 20

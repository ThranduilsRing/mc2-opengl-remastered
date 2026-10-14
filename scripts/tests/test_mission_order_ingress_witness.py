"""Offline transport and timing proofs; no engine or desktop interaction."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mission_order_ingress_witness as subject
import mc2_cmd


class Clock:
    def __init__(self):
        self.time = 0.0

    def now(self):
        return self.time

    def sleep(self, seconds):
        self.time += seconds


class Smoke:
    def __init__(self, clock, exit_at=float("inf")):
        self.clock = clock
        self.exit_at = exit_at

    def poll(self):
        return 0 if self.clock.now() >= self.exit_at else None


def reply(ok=True, error=None):
    return {"ok": ok, "error": error, "version": 1, "data": {}}


def test_lease_and_slow_load_do_not_spend_play_budget(capsys):
    clock = Clock()
    commands = []

    def send(command):
        commands.append(command)
        if clock.now() < 180:
            raise ConnectionRefusedError("listener not ready")
        clock.sleep(0.1)
        return reply()

    assert subject.witness(Smoke(clock, 210), send, clock.now, clock.sleep)
    assert commands.count("order_ingress_fixture_submit") == 3
    assert commands.count("order_ingress_fixture_check") == 3
    assert clock.now() < 181
    output = capsys.readouterr().out
    assert "ConnectionRefusedError" in output
    assert "batch 3/3 installed and automatically retired" in output


def test_readiness_has_a_deadline_and_reports_transport_errors(capsys):
    clock = Clock()

    def send(command):
        assert command == "ping"
        raise OSError("socket unavailable")

    assert not subject.witness(Smoke(clock), send, clock.now, clock.sleep, startup_timeout=2)
    assert clock.now() == 2
    assert "socket unavailable" in capsys.readouterr().out


@pytest.mark.parametrize("result", [reply(False, "unknown command"), None, "not json"])
def test_submit_rejection_or_ambiguous_transport_is_never_replayed(result, capsys):
    clock = Clock()
    commands = []

    def send(command):
        commands.append(command)
        if command == "ping":
            return reply()
        if result is None:
            raise TimeoutError("reply lost")
        return result

    assert not subject.witness(Smoke(clock), send, clock.now, clock.sleep)
    assert commands == ["ping", "order_ingress_fixture_submit"]
    output = capsys.readouterr().out
    assert "unknown command" in output or "reply lost" in output or "invalid dev-shell" in output


@pytest.mark.parametrize("error", ["receipts Pending", "receipts retained", "completion suppressed"])
def test_negative_controls_cannot_pass_or_submit_another_batch(error, capsys):
    clock = Clock()
    commands = []

    def send(command):
        commands.append(command)
        return reply(False, error) if command.endswith("_check") else reply()

    assert not subject.witness(Smoke(clock), send, clock.now, clock.sleep, runtime_timeout=2)
    assert clock.now() == 2
    assert commands.count("order_ingress_fixture_submit") == 1
    output = capsys.readouterr().out
    assert error in output
    assert "automatically retired" not in output


def test_all_batches_share_the_play_deadline(capsys):
    clock = Clock()
    checks = 0

    def send(command):
        nonlocal checks
        if command.endswith("_check"):
            checks += 1
            clock.sleep(1.1)
        return reply()

    assert not subject.witness(Smoke(clock), send, clock.now, clock.sleep, runtime_timeout=2)
    assert checks == 2
    assert "batch 2/3 installed" not in capsys.readouterr().out


def test_clean_smoke_exit_alone_is_not_witness_success(capsys):
    clock = Clock()

    def send(command):
        raise ValueError("empty reply")

    assert not subject.witness(Smoke(clock, 1), send, clock.now, clock.sleep)
    assert "empty reply" in capsys.readouterr().out


def test_client_uses_selected_port_ndjson_and_receives_a_fragmented_reply(monkeypatch):
    received = iter([b'{"ok":true,"error":null,', b'"version":1,"data":{}}\n'])
    requests = []

    class Connection:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

        def sendall(self, request):
            requests.append(request)

        def recv(self, size):
            return next(received)

    def connect(address, timeout):
        assert address == ("127.0.0.1", 19874)
        assert timeout == 10
        return Connection()

    monkeypatch.setattr(mc2_cmd.socket, "create_connection", connect)
    assert mc2_cmd.send_command("order_ingress_fixture_submit", port=19874, timeout=10)["ok"]
    assert requests[0].endswith(b"\n")
    assert json.loads(requests[0]) == {"type": "order_ingress_fixture_submit", "params": {}}

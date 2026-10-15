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


@pytest.mark.parametrize("result", [None, "not json", {"ok": False, "version": 2}])
def test_ambiguous_transport_is_never_replayed(result, capsys):
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
    assert "reply lost" in output or "invalid dev-shell" in output
    assert "submission ambiguous: not replaying" in output


def test_explicit_rejections_retry_until_eligible_and_log_every_attempt(capsys):
    clock = Clock()
    submissions = 0

    def send(command):
        nonlocal submissions
        if command.endswith("_submit"):
            submissions += 1
            if submissions <= 3:
                return reply(False, "no eligible recipient")
        return reply()

    assert subject.witness(Smoke(clock), send, clock.now, clock.sleep)
    assert submissions == 6
    assert clock.now() == 0.75
    output = capsys.readouterr().out
    attempts = [line for line in output.splitlines() if "command=order_ingress_fixture_submit" in line]
    assert len(attempts) == 6
    assert all(f"batch=1 attempt={i + 1}" in attempts[i] for i in range(4))
    assert "batch 3/3 installed and automatically retired" in output


@pytest.mark.parametrize("exit_at, expected_time, expected_attempts", [(float("inf"), 1, 4), (0.5, 0.5, 2)])
def test_explicit_rejections_stop_at_deadline_or_harness_exit(exit_at, expected_time, expected_attempts, capsys):
    clock = Clock()
    commands = []

    def send(command):
        commands.append(command)
        return reply() if command == "ping" else reply(False, "no eligible recipient")

    assert not subject.witness(Smoke(clock, exit_at), send, clock.now, clock.sleep, runtime_timeout=1)
    assert clock.now() == expected_time
    assert commands.count("order_ingress_fixture_submit") == expected_attempts
    assert "order_ingress_fixture_check" not in commands
    output = capsys.readouterr().out
    assert output.count("command=order_ingress_fixture_submit") == expected_attempts
    assert "submission failed: harness exited or play deadline expired" in output


def test_rejection_then_lost_reply_is_not_replayed(capsys):
    clock = Clock()
    submissions = 0

    def send(command):
        nonlocal submissions
        if command.endswith("_submit"):
            submissions += 1
            if submissions == 1:
                return reply(False, "not ready")
            raise TimeoutError("reply lost")
        return reply()

    assert not subject.witness(Smoke(clock), send, clock.now, clock.sleep)
    assert submissions == 2
    assert "submission ambiguous: not replaying" in capsys.readouterr().out


def test_retry_uses_time_remaining_after_previous_batch(capsys):
    clock = Clock()
    submissions = 0

    def send(command):
        nonlocal submissions
        if command.endswith("_submit"):
            submissions += 1
            return reply(submissions == 1, "no eligible recipient")
        if command.endswith("_check"):
            clock.sleep(0.9)
        return reply()

    assert not subject.witness(Smoke(clock), send, clock.now, clock.sleep, runtime_timeout=1)
    assert clock.now() == 1
    assert submissions == 2
    output = capsys.readouterr().out
    assert "batch 1/3 installed" in output
    assert "batch 2/3 submission failed" in output


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


STOCK = """FITini
[Warriors]
ul NumWarriors = 2
[Parts]
ul NumParts = 2
[Warrior1]
st Brain = "stock"
[Part1]
c CommanderID = 1
ul Pilot = 1
ul SquadNum = 9
l Active = 1
l Exists = 1
f PositionX = 32
f PositionY = -48
l[15] IndicesOfAlternatives = -1,-1,
[Warrior2]
st Brain = "stock"
[Part2]
c CommanderID = 0
ul Pilot = 2
ul SquadNum = 10
l Active = 1
l Exists = 1
f PositionX = 0
f PositionY = 0
l[15] IndicesOfAlternatives = -1,-1,
"""


@pytest.mark.parametrize("case", ["valid", "malformed", "suppressed"])
def test_authored_fixture_forces_an_alternate_and_preserves_original_pilot(case):
    text, expected = subject.authored_fixture(STOCK + "FITend\n", case)
    assert expected == {"status": "Installed", "original": 1, "selected": 3, "pilot": 1,
                        "stance": 4, "kind": 7, "points": [[32,-48,0,0], [48,-32,0,0]]}
    assert "IndicesOfAlternatives = 3," in text
    assert "ul Squad0 = 1" in text and "ul Squad1 = 0" in text
    alternate = text.split("[Part3]", 1)[1].split("[Squads]", 1)[0]
    assert "ul Pilot = 2" in alternate
    assert "l OrderType = 2" in alternate
    assert ("Waypoint2X" in alternate) == (case == "malformed")
    assert ('compatibilityMode = "Enhanced"' in text) == (case == "suppressed")
    assert text.count("FITend") == 1 and text.endswith("FITend\n")


@pytest.mark.parametrize("case", ["valid", "malformed", "suppressed"])
def test_authored_witness_rejects_disconnected_loader_and_duplicate_diagnostics(case):
    _, expected = subject.authored_fixture(STOCK, case)
    empty = {"installations": [], "diagnostics": []}
    assert not subject.authored_evidence_matches(empty, expected, case)
    if case == "valid":
        good = {"installations": [expected], "diagnostics": []}
    else:
        diagnostic = {"status": "Rejected" if case == "malformed" else "Suppressed",
                      "original": 1, "selected": 3, "pilot": 1,
                      "reason": "stray_key" if case == "malformed" else "enhanced_opord"}
        good = {"installations": [], "diagnostics": [diagnostic]}
        assert not subject.authored_evidence_matches({**good, "diagnostics": [diagnostic] * 2}, expected, case)
    assert subject.authored_evidence_matches(good, expected, case)
    clock = Clock()
    assert not subject.authored_witness(Smoke(clock), lambda command: {**reply(), "data": empty},
                                        expected, case, clock.now, clock.sleep, runtime_timeout=1)
    assert clock.now() == 1


def test_authored_evidence_checks_every_point_mode_identity_and_stance():
    import copy
    _, expected = subject.authored_fixture(STOCK, "valid")
    for field in ("original", "selected", "pilot", "stance", "kind", "points", "mode"):
        wrong = copy.deepcopy(expected)
        if field in ("points", "mode"):
            wrong["points"][1][3 if field == "mode" else 0] += 1
        else:
            wrong[field] += 1
        assert not subject.authored_evidence_matches({"installations": [wrong], "diagnostics": []}, expected, "valid")


def test_authored_waits_for_bound_mission_before_spending_play_budget():
    clock = Clock()
    _, expected = subject.authored_fixture(STOCK, "valid")

    def send(command):
        assert command == "authored_order_evidence"
        if clock.now() < 180:
            return reply(False, "command not registered until mission binding")
        return {**reply(), "data": {"installations": [expected], "diagnostics": []}}

    assert subject.authored_witness(Smoke(clock, 210), send, expected, "valid", clock.now, clock.sleep)
    assert clock.now() == 180


def test_fractional_authored_coordinate_survives_engine_float32_json_round_trip():
    import copy
    stock = STOCK.replace("PositionX = 32", "PositionX = 2880.000244")
    _, expected = subject.authored_fixture(stock, "valid")
    actual = copy.deepcopy(expected)
    actual["points"][0][0] = 2880.0002
    actual["points"][1][0] = 2896.0002
    assert actual != expected
    assert subject.authored_evidence_matches({"installations": [actual], "diagnostics": []}, expected, "valid")
    for field in ("original", "selected", "pilot", "stance", "kind"):
        wrong = copy.deepcopy(actual)
        wrong[field] += 1
        assert not subject.authored_evidence_matches({"installations": [wrong], "diagnostics": []}, expected, "valid")
    wrong = copy.deepcopy(actual)
    wrong["points"][0][3] += 1
    assert not subject.authored_evidence_matches({"installations": [wrong], "diagnostics": []}, expected, "valid")


@pytest.mark.parametrize("wrong", [2880.0, 2880.00048828125, 2881.0, float("nan"), float("inf"), True])
def test_fractional_coordinate_negative_control_rejects_wrong_float32(wrong):
    import copy
    _, expected = subject.authored_fixture(STOCK.replace("PositionX = 32", "PositionX = 2880.000244"), "valid")
    actual = copy.deepcopy(expected)
    actual["points"][0][0] = wrong
    assert not subject.authored_evidence_matches({"installations": [actual], "diagnostics": []}, expected, "valid")

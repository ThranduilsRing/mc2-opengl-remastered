"""Real loopback socket framing, authentication and harness lifecycle tests."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "visual_verify"))
import console_child  # noqa: E402
from smoke_lib.runtime_bridge import MAX_FRAME, RuntimeBridge, receive_frame, send_frame


def test_fragmented_utf8_frame():
    left, right = socket.socketpair()
    message = {"name": "pilot \u2603", "version": 1}
    body = json.dumps(message, ensure_ascii=False).encode("utf-8")
    def write():
        with left:
            for byte in struct.pack("!I", len(body)) + body:
                left.sendall(bytes([byte]))
    thread = threading.Thread(target=write)
    thread.start()
    try:
        with right:
            right.settimeout(2)
            assert receive_frame(right) == message
    finally:
        thread.join()


@pytest.mark.parametrize("size", [0, MAX_FRAME + 1, 0xffffffff])
def test_invalid_length_before_allocation(size):
    left, right = socket.socketpair()
    with left, right:
        left.sendall(struct.pack("!I", size))
        with pytest.raises(ValueError, match="length"):
            receive_frame(right)


@pytest.mark.parametrize("body", [b"[]", b"null", b"{", b'{"bad":NaN}', b'\xff'])
def test_invalid_json(body):
    left, right = socket.socketpair()
    with left, right:
        left.sendall(struct.pack("!I", len(body)) + body)
        with pytest.raises(ValueError):
            receive_frame(right)


def test_harness_hello_subscribe_reconnect_and_persist(tmp_path):
    output = tmp_path / "debug_state/latest_runtime_bridge.json"
    with RuntimeBridge(output) as bridge:
        env = bridge.environment
        assert set(env) == {"MC2_BRIDGE", "MC2_BRIDGE_PORT", "MC2_BRIDGE_SESSION"}
        def connect(session):
            connection = socket.create_connection(("127.0.0.1", int(env["MC2_BRIDGE_PORT"])), timeout=2)
            send_frame(connection, dict(version=1, type="hello", session=session, read_only=True))
            return connection
        with connect(bridge.session) as connection:
            subscribe = receive_frame(connection)
            assert subscribe["type"] == "subscribe"
            assert subscribe["session"] == bridge.session
            assert subscribe["streams"] == ["runtime.observation", "mover.transform"]
            send_frame(connection, dict(version=1, type="telemetry", session=bridge.session,
                                       stream="runtime.observation", data={"frame": 123}))
        # A second handshake proves the receiver consumed all of the first connection.
        with connect(bridge.session) as connection:
            assert receive_frame(connection)["type"] == "subscribe"
    saved = json.loads(output.read_text())
    assert saved["runtime.observation"]["data"]["frame"] == 123
    assert saved["hello"]["session"] == bridge.session
    assert not bridge.thread.is_alive()


def test_stale_hello_is_rejected(tmp_path):
    with RuntimeBridge(tmp_path / "state.json") as bridge:
        port = int(bridge.environment["MC2_BRIDGE_PORT"])
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            send_frame(connection, dict(version=1, type="hello", session="stale", read_only=True))
            assert connection.recv(1) == b""
    assert "mismatch" in bridge.error


def test_idle_loading_does_not_disconnect_or_prevent_shutdown(tmp_path):
    bridge = RuntimeBridge(tmp_path / "state.json")
    connection = None
    try:
        with bridge:
            port = int(bridge.environment["MC2_BRIDGE_PORT"])
            connection = socket.create_connection(("127.0.0.1", port), timeout=3)
            send_frame(connection, dict(version=1, type="hello", session=bridge.session, read_only=True))
            assert receive_frame(connection)["type"] == "subscribe"
            connection.settimeout(2.2)
            # A stalled mission load sends no samples. The parent must stay connected.
            with pytest.raises(socket.timeout):
                connection.recv(1)
        assert connection.recv(1) == b""  # harness shutdown interrupts its blocked read
    finally:
        if connection:
            connection.close()
    assert not bridge.thread.is_alive()


def _fake_game(port, session):
    """A stand-in for the game control loop: pause holds the counter, step N adds N, others are rejected."""
    state = {"ticks": 100, "paused": False, "pending": 0}
    def control():
        return dict(paused=state["paused"], ticks=state["ticks"], pending_steps=state["pending"], speed=0.0)
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    send_frame(connection, dict(version=1, type="hello", session=session, read_only=False))
    receive_frame(connection)
    connection.settimeout(0.05)
    def run():
        while True:
            try:
                message = receive_frame(connection)
            except socket.timeout:
                if not state["paused"]:
                    state["ticks"] += 3
                try:
                    send_frame(connection, dict(version=1, type="telemetry", session=session,
                                               stream="runtime.observation", data={}, control=control()))
                except OSError:
                    return
                continue
            except (EOFError, OSError, ValueError):
                return
            reply = dict(version=1, session=session, seq=message.get("seq"))
            cmd = message.get("cmd")
            if cmd == "pause":
                state["paused"] = True; reply["type"] = "ack"
            elif cmd == "resume":
                state["paused"] = False; reply["type"] = "ack"
            elif cmd == "step" and state["paused"]:
                state["ticks"] += message["n"]; reply["type"] = "ack"
            elif cmd == "set_speed":
                reply["type"] = "ack"
            else:
                reply.update(type="nack", error="unknown_command")
            reply["control"] = control()
            try:
                send_frame(connection, reply)
            except OSError:
                return
    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return connection, thread


def test_scripted_control_run_records_ticks_and_rejections(tmp_path):
    import time
    script = "pause;hold:0.3;step:5;resume;run:0.3;bogus"
    with RuntimeBridge(tmp_path / "state.json", script) as bridge:
        port = int(bridge.environment["MC2_BRIDGE_PORT"])
        connection, thread = _fake_game(port, bridge.session)
        deadline = time.time() + 20
        while len(bridge.control_log) < 6 and time.time() < deadline:
            time.sleep(0.1)
        connection.close()
    log = {entry["op"]: entry for entry in bridge.control_log}
    assert log["pause"]["reply"]["type"] == "ack"
    assert log["hold"]["ticks_after"] == log["hold"]["ticks_before"]          # pause holds the counter
    assert log["step"]["ticks_after"] - log["step"]["ticks_before"] == 5      # stepped == requested
    assert log["run"]["ticks_after"] > log["run"]["ticks_before"]             # resume runs again
    assert log["bogus"]["reply"]["type"] == "nack"
    assert log["bogus"]["reply"]["error"] == "unknown_command"


def test_script_rejects_unknown_ops():
    with pytest.raises(ValueError):
        RuntimeBridge(Path("unused.json"), "pause;eval:1")


@pytest.mark.skipif(not os.environ.get("MC2_BRIDGE_TEST_EXE"),
                    reason="live run: set MC2_BRIDGE_TEST_EXE to a deployed mc2.exe (Windows, off-screen via run_smoke)")
def test_live_control_run_steps_exactly_and_pause_holds():
    exe = Path(os.environ["MC2_BRIDGE_TEST_EXE"])
    script = "pause;hold:1.5;step:5;hold:1;step:12;resume;run:1.5;pause;set_speed:2;bogus"
    run = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / "run_smoke.py"),
                          "--mission", "mc2_10", "--duration", "45", "--keep-logs", "--exe", str(exe),
                          "--runtime-bridge-script", script],
                         capture_output=True, text=True, timeout=600, **console_child.child_kwargs(redirected=True))
    saved = json.loads((exe.parent / "debug_state/latest_runtime_bridge.json").read_text())
    assert saved["error"] is None, run.stderr[-2000:]
    log = saved["control_log"]
    assert [e["op"] for e in log] == ["pause", "hold", "step", "hold", "step", "resume", "run", "pause", "set_speed", "bogus"]
    assert log[1]["ticks_after"] == log[1]["ticks_before"]
    assert log[2]["ticks_after"] - log[2]["ticks_before"] == 5
    assert log[3]["ticks_after"] == log[3]["ticks_before"]
    assert log[4]["ticks_after"] - log[4]["ticks_before"] == 12
    assert log[6]["ticks_after"] > log[6]["ticks_before"]
    assert log[8]["reply"]["type"] == "ack"
    assert log[9]["reply"]["type"] == "nack" and log[9]["reply"]["error"] == "unknown_command"

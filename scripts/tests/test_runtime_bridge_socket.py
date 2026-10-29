"""Real loopback socket framing, authentication and harness lifecycle tests."""
import json
from pathlib import Path
import socket
import struct
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
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

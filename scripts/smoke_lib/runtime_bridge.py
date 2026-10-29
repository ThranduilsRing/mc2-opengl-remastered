"""Harness-owned, loopback-only receiver for the read-only runtime bridge v1."""
import json
import socket
import struct
import threading
import time
import uuid

MAX_FRAME = 1024 * 1024


def send_frame(connection, message):
    body = json.dumps(message, allow_nan=False).encode("utf-8")
    if not 0 < len(body) <= MAX_FRAME:
        raise ValueError("invalid bridge frame length")
    connection.sendall(struct.pack("!I", len(body)) + body)


def receive_frame(connection, stop=None):
    def exact(size):
        chunks = bytearray()
        while len(chunks) < size:
            if stop is not None and stop.is_set():
                raise EOFError("bridge stopped")
            try:
                part = connection.recv(size - len(chunks))
            except socket.timeout:
                if stop is not None:
                    continue  # preserve partial-frame progress through idle loading
                raise
            if not part:
                raise EOFError("bridge disconnected")
            chunks.extend(part)
        return bytes(chunks)

    size, = struct.unpack("!I", exact(4))
    if not 0 < size <= MAX_FRAME:
        raise ValueError("invalid bridge frame length")
    message = json.loads(exact(size).decode("utf-8"),
                         parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    if not isinstance(message, dict):
        raise ValueError("bridge frame must be an object")
    return message


class RuntimeBridge:
    def __init__(self, output):
        self.output = output
        self.session = str(uuid.uuid4())
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.2)
        self.stop = threading.Event()
        self.connection = None
        self.thread = None
        self.latest = {}
        self.error = None

    @property
    def environment(self):
        return {"MC2_BRIDGE": "1", "MC2_BRIDGE_PORT": str(self.listener.getsockname()[1]),
                "MC2_BRIDGE_SESSION": self.session}

    def __enter__(self):
        self.thread = threading.Thread(target=self._run, name="mc2-bridge-receiver")
        self.thread.start()
        return self

    def _run(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.connection = connection
            try:
                with connection:
                    connection.settimeout(2)
                    hello = receive_frame(connection)
                    if (hello.get("type") != "hello" or hello.get("version") != 1 or
                            hello.get("session") != self.session or hello.get("read_only") is not True):
                        raise ValueError("bridge hello/session/version mismatch")
                    self.latest["hello"] = hello
                    send_frame(connection, {"version": 1, "session": self.session, "type": "subscribe",
                               "seq": 1, "hz": 2, "streams": ["runtime.observation", "mover.transform"]})
                    # Loading can stall main-thread publication for many seconds.
                    # Bounded reads let shutdown join reliably on Windows too.
                    connection.settimeout(0.2)
                    while not self.stop.is_set():
                        message = receive_frame(connection, self.stop)
                        if message.get("version") != 1 or message.get("session") != self.session:
                            raise ValueError("bridge telemetry/session/version mismatch")
                        if message.get("type") == "telemetry":
                            stream = message.get("stream")
                            if stream in ("runtime.observation", "mover.transform"):
                                self.latest[stream] = message
                                self.latest["written_at_epoch"] = time.time()
            except ValueError as error:
                # Keep protocol failures even if harness shutdown races the peer's EOF.
                self.error = str(error)
            except (OSError, EOFError) as error:
                if not self.stop.is_set():
                    self.error = str(error)
            finally:
                self.connection = None

    def __exit__(self, *unused):
        self.stop.set()
        self.listener.close()
        connection = self.connection
        if connection:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self.thread:
            self.thread.join()
        self.output.parent.mkdir(parents=True, exist_ok=True)
        state = {"session": self.session, "error": self.error, **self.latest}
        temporary = self.output.with_suffix(".tmp")
        temporary.write_text(json.dumps(state, indent=2), encoding="utf-8")
        temporary.replace(self.output)

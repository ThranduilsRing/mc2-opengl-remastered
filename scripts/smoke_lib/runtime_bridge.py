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


def parse_script(text):
    """'pause;step:5;hold:1;resume;set_speed:2;bogus' -> list of (op, arg). Ops are the control slice 1 verbs plus
    hold/run timing steps and 'bogus' (an unknown command the game must reject)."""
    ops = []
    for item in filter(None, (part.strip() for part in text.split(";"))):
        op, _, arg = item.partition(":")
        if op not in ("pause", "resume", "step", "set_speed", "hold", "run", "bogus"):
            raise ValueError(f"unknown bridge script op {op!r}")
        ops.append((op, arg))
    return ops


class RuntimeBridge:
    def __init__(self, output, script=None):
        self.output = output
        self.script = parse_script(script) if isinstance(script, str) else (script or [])
        self.hz = 10 if self.script else 2
        self.seq = 1
        self.control = {}
        self.control_log = []
        self.acks = {}
        self.cond = threading.Condition()
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
                            hello.get("session") != self.session or not isinstance(hello.get("read_only"), bool)):
                        raise ValueError("bridge hello/session/version mismatch")
                    self.latest["hello"] = hello
                    send_frame(connection, {"version": 1, "session": self.session, "type": "subscribe",
                               "seq": 1, "hz": self.hz, "streams": ["runtime.observation", "mover.transform"]})
                    # Loading can stall main-thread publication for many seconds.
                    # Bounded reads let shutdown join reliably on Windows too.
                    connection.settimeout(0.2)
                    driver = None
                    if self.script:
                        driver = threading.Thread(target=self._drive, args=(connection,), name="mc2-bridge-script")
                        driver.start()
                    while not self.stop.is_set():
                        message = receive_frame(connection, self.stop)
                        if message.get("version") != 1 or message.get("session") != self.session:
                            raise ValueError("bridge telemetry/session/version mismatch")
                        if isinstance(message.get("control"), dict):
                            with self.cond:
                                self.control = message["control"]
                                if message.get("type") in ("ack", "nack"):
                                    self.acks[message.get("seq")] = message
                                self.cond.notify_all()
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

    def _ticks(self):
        return self.control.get("ticks")

    def _wait(self, predicate, timeout):
        with self.cond:
            return self.cond.wait_for(lambda: self.stop.is_set() or predicate(), timeout) and not self.stop.is_set()

    def _command(self, connection, cmd, **fields):
        with self.cond:
            seq = self.seq = self.seq + 1
        send_frame(connection, dict(version=1, session=self.session, type="command", seq=seq, cmd=cmd, **fields))
        self._wait(lambda: seq in self.acks, 5)
        return self.acks.get(seq)

    def _pause(self, seconds):
        self.stop.wait(seconds)

    def _drive(self, connection):
        """Scripted control run: record what each command did to the sim tick counter."""
        try:
            # The mission must be ticking before the script means anything.
            if not self._wait(lambda: (self._ticks() or 0) > 5, 180):
                self.control_log.append({"op": "start", "error": "mission never ticked"})
                return
            for op, arg in self.script:
                entry = {"op": op, "arg": arg, "ticks_before": self._ticks()}
                if op == "pause":
                    entry["reply"] = self._command(connection, "pause")
                    self._pause(0.8)   # let the frame the pause raced with finish
                    entry["ticks_before"] = self._ticks()
                elif op == "resume":
                    entry["reply"] = self._command(connection, "resume")
                elif op == "step":
                    entry["reply"] = self._command(connection, "step", n=int(arg))
                    target = entry["ticks_before"] + int(arg)
                    self._wait(lambda: (self._ticks() or 0) >= target, 10)
                    self._pause(0.8)   # an overrun would show up here
                elif op == "set_speed":
                    entry["reply"] = self._command(connection, "set_speed", speed=float(arg))
                elif op == "bogus":
                    entry["reply"] = self._command(connection, "eval", code="1")
                elif op in ("hold", "run"):
                    self._pause(float(arg or 1))
                entry["ticks_after"] = self._ticks()
                with self.cond:
                    entry["control"] = dict(self.control)
                self.control_log.append(entry)
        except (OSError, ValueError) as error:
            self.control_log.append({"op": "error", "error": str(error)})

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
        state = {"session": self.session, "error": self.error, "control_log": self.control_log, **self.latest}
        temporary = self.output.with_suffix(".tmp")
        temporary.write_text(json.dumps(state, indent=2), encoding="utf-8")
        temporary.replace(self.output)

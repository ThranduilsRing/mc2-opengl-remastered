# scripts/smoke_lib/host_lease.py
"""FIFO engine slots shared across worktrees, with exclusive reservations for timing proofs.

MC2_HOST_CORES_PER_SLOT selects physical cores per slot (default 2).
MC2_HOST_GAME_SLOTS selects 1..6/max(2,width) slots (default 2); cores 0..1 stay reserved.
MC2_HOST_LEASE_EXCLUSIVE=1 reserves every slot. Batch hold/release keeps a reservation
across a serial chain. Dead holders and tickets recover automatically. Private paths
isolate tests; disabling leases requires MC2_OWNER_NO_LEASE=1.
"""
from __future__ import annotations

import argparse
import atexit
import json
import os
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, List, Optional, Tuple

try:
    from . import bounded_wait, child_registry
except ImportError:                       # imported as a top-level module from scripts/
    import bounded_wait                   # type: ignore
    import child_registry                 # type: ignore

# Slot zero retains the host-global path; the queue and other slots live beside it.
LEASE_PATH = Path("A:/Games/mc2-opengl/.host_game_lease.json")
if sys.platform == "linux":
    LEASE_PATH = Path.home() / ".cache/mc2/host_game_lease.json"
PATH_ENV = "MC2_HOST_LEASE_PATH"          # tests only: a private lease file (the conftests set one per pytest process)
OFF_ENV = "MC2_HOST_LEASE"                # "off" switches leasing off, owner only (see OWNER_ENV)
OWNER_ENV = "MC2_OWNER_NO_LEASE"          # "1" authorises OFF_ENV; the same owner switch run_smoke --no-lease needs
TIMEOUT_ENV = "MC2_HOST_LEASE_TIMEOUT"    # seconds, default 3600
BATCH_ENV = "MC2_HOST_LEASE_BATCH"        # the tag of a `hold` batch lease this process's launches run under
DEFAULT_BATCH_TTL_S = 7200.0
MAX_BATCH_TTL_S = 6 * 3600.0              # the lane cap: no batch outlives a lane session
DEFAULT_TIMEOUT_S = 3600.0
POLL_S = 2.0
REPORT_EVERY_S = 30.0

def cores_per_slot() -> int:
    width = int(os.environ.get("MC2_HOST_CORES_PER_SLOT", "2"))
    if not 1 <= width <= 6:
        raise ValueError("MC2_HOST_CORES_PER_SLOT must be between 1 and 6")
    return width


def slot_count() -> int:
    maximum = 6 // max(2, cores_per_slot())
    # Two slots limit host load because three can trip the heartbeat_freeze_play smoke gate.
    # Three remains an env override for sim-only or strict runs that grade no smoke gates.
    count = int(os.environ.get("MC2_HOST_GAME_SLOTS", "2"))
    if not 1 <= count <= maximum:
        raise ValueError(f"MC2_HOST_GAME_SLOTS must be between 1 and {maximum}")
    return count


def slot_path(slot: int) -> Path:
    path = lease_path()
    return path if slot == 0 else path.with_name(path.stem + f".slot{slot}" + path.suffix)


def affinity_mask(slot: int) -> int:
    width = cores_per_slot()
    if not 0 <= slot < 6 // max(2, width):
        raise ValueError("slot exceeds the physical game core budget")
    return ((1 << (width * 2)) - 1) << (4 + slot * max(2, width) * 2)


@contextmanager
def _queue_lock():
    path = lease_path().with_suffix(".queue.lock")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as fh:
        if sys.platform == "win32":
            import msvcrt
            if fh.tell() == 0:
                fh.write(b"0")
                fh.flush()
            while True:
                fh.seek(0)
                try:
                    msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    time.sleep(0.02)
        else:
            import fcntl
            fcntl.flock(fh, fcntl.LOCK_EX)
        try:
            yield
        finally:
            if sys.platform == "win32":
                fh.seek(0)
                msvcrt.locking(fh.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(fh, fcntl.LOCK_UN)


def _records():
    records = [(slot_path(i), _read(slot_path(i))) for i in range(6)]
    return [(p, None if r and r.get("legacy_guard") else r) for p, r in records]


def _remove_records(records):
    """Keep a legacy-visible owner until the last slot releases."""
    removed = {p for p, r in records if _read(p) == r}
    survivors = [(p, r) for p, r in _records() if p not in removed and _holder_alive(r)]
    global_path = lease_path()
    global_rec = _read(global_path)
    guard = global_rec and global_rec.get("legacy_guard")
    if survivors and not global_path.exists():
        taken, _ = _try_take(global_path, "slot compatibility guard")
        guard = taken
    if survivors and (global_path in removed or guard):
        _write_record(global_path, dict(survivors[0][1], legacy_guard=True))
        removed.discard(global_path)
    for p in removed:
        _retry_file_change(p.unlink, missing_ok=True)
    if not survivors and guard:
        _retry_file_change(global_path.unlink, missing_ok=True)


def _ours(rec):
    return bool(rec and _holder_alive(rec) and (rec.get("pid") == os.getpid() or _batch_member(rec)))


def _exclusive(value=None):
    return os.environ.get("MC2_HOST_LEASE_EXCLUSIVE") == "1" if value is None else value


def apply_affinity(proc) -> None:
    """Pin an engine or its launcher; descendants inherit the core reservation."""
    records = [rec for _, rec in _records() if _ours(rec)]
    if not records:
        return
    mask = 0
    for rec in records:
        mask |= rec.get("affinity_mask", 3 << (4 + rec.get("slot", 0) * 2))
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        kernel.SetProcessAffinityMask.restype = wintypes.BOOL
        if not kernel.SetProcessAffinityMask(int(proc._handle), mask):
            proc.kill()
            raise ctypes.WinError(ctypes.get_last_error())
        kernel.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        kernel.GetProcessAffinityMask.restype = wintypes.BOOL
        actual, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(int(proc._handle), ctypes.byref(actual), ctypes.byref(system)):
            proc.kill()
            raise ctypes.WinError(ctypes.get_last_error())
        if actual.value != mask:
            proc.kill()
            raise RuntimeError(f"engine affinity mismatch: requested {mask:#x}, applied {actual.value:#x}")
    elif hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(proc.pid, {i for i in range(16) if mask & (1 << i)})
    print(f"[host-affinity] pid={proc.pid} mask={mask:#x} logical_cpus={mask.bit_count()}", file=sys.stderr, flush=True)


class HostLeaseBusy(RuntimeError):
    """Another lane holds the host lease, or a game this process did not start is still running."""


def _log_default(m: str) -> None:
    print(m, file=sys.stderr, flush=True)


def redirected() -> bool:
    """True when a private lease file is in use (a test): the host's real engines are then not this process's business."""
    return bool(os.environ.get(PATH_ENV))


def lease_path() -> Path:
    override = os.environ.get(PATH_ENV)
    if override:
        return Path(override)
    if "pytest" in sys.modules:
        raise RuntimeError(f"a test reached the real host lease {LEASE_PATH}; the conftest.py files set {PATH_ENV} to a private file")
    return LEASE_PATH


_off_logged = False


def disabled(log: Callable[[str], None] = _log_default) -> bool:
    """True only for the owner: MC2_HOST_LEASE=off with MC2_OWNER_NO_LEASE=1. Never follows the cursor-guard test switch."""
    global _off_logged
    if os.environ.get(OFF_ENV, "").lower() != "off":
        return False
    if os.environ.get(OWNER_ENV) == "1":
        return True
    if not _off_logged:
        _off_logged = True
        log(f"[host-lease] {OFF_ENV}=off ignored: it needs {OWNER_ENV}=1 (owner only); leasing as usual")
    return False


def _read(path: Path) -> Optional[dict]:
    try:
        data = path.read_bytes()
        value = json.loads(data.decode("utf-8"))
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


def _holder_alive(rec: Optional[dict]) -> bool:
    if not rec:
        return False
    try:
        pid = int(rec.get("pid", 0))
    except (TypeError, ValueError):
        return False
    if not child_registry.pid_alive(pid):
        return False
    created = rec.get("created")
    now = child_registry.process_create_time(pid)
    return created is None or now is None or str(created) == str(now)


def _try_take(path: Path, label: str) -> Tuple[bool, Optional[dict]]:
    """Claim a complete record so legacy readers never mistake publication for a dead owner."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".{os.getpid()}.take.tmp")
    tmp.write_text(json.dumps({"pid": os.getpid(), "created": child_registry.process_create_time(os.getpid()),
                               "label": label, "cwd": os.getcwd(), "argv": sys.argv[:3], "since": time.time()}), encoding="utf-8")
    try:
        for _ in range(3):
            try:
                os.link(tmp, path)
            except FileExistsError:
                rec = _read(path)
                if rec is None:
                    return False, None
                if rec.get("pid") == os.getpid() or (_batch_member(rec) and _holder_alive(rec)):
                    return True, None
                if _holder_alive(rec):
                    return False, rec
                if _read(path) == rec:
                    _retry_file_change(path.unlink, missing_ok=True)
                continue
            return True, None
        return False, _read(path)
    finally:
        _retry_file_change(tmp.unlink, missing_ok=True)


def release() -> None:
    """Give the lease back if this process holds it."""
    with _queue_lock():
        _remove_records([(p, r) for p, r in _records() if r and r.get("pid") == os.getpid()])


_registered = False


def holds() -> bool:
    """True when this process already holds the lease, itself or as a member of the live batch named by MC2_HOST_LEASE_BATCH."""
    return any(_ours(rec) for _, rec in _records())


def _batch_member(rec: Optional[dict]) -> bool:
    """True when `rec` is a batch lease whose tag this process carries in MC2_HOST_LEASE_BATCH."""
    tag = os.environ.get(BATCH_ENV, "")
    return bool(rec and tag and rec.get("batch") == tag)


def _describe(rec: Optional[dict]) -> str:
    if not rec:
        return "unknown holder"
    return (f"{rec.get('label', '?')} (pid {rec.get('pid')}, {' '.join(map(str, rec.get('argv', [])))}"
            f"{', in ' + str(rec['cwd']) if rec.get('cwd') else ''})")


def foreign_engines() -> List[Tuple[int, str]]:
    """Engines running on the host as (pid, image). Empty under a private lease file: a test never looks at the real host."""
    if redirected():
        return []
    return child_registry.engine_processes()


def acquire(label: str, *, wait: bool = True, timeout_s: Optional[float] = None, kill_orphans: bool = False,
            log: Callable[[str], None] = _log_default,
            sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic,
            scan: Optional[Callable[..., List[Tuple[int, str]]]] = None, exclusive: Optional[bool] = None) -> None:
    """Hold the host lease, or raise HostLeaseBusy. See the module docstring for the rules."""
    global _registered
    if disabled(log):
        log(f"[host-lease] OFF ({OFF_ENV}=off, owner): {label} launches without the host lease")
        return
    exclusive = _exclusive(exclusive) or any(word in (label + " " + sys.argv[0]).lower()
                                            for word in ("perf", "fps", "frame-time", "frametime", "tracy"))
    if holds():
        if exclusive and not all(_ours(_read(slot_path(i))) for i in range(slot_count())):
            raise HostLeaseBusy("cannot upgrade a held normal slot; release it before requesting exclusive")
        return
    scan = scan or foreign_engines
    if timeout_s is None:
        timeout_s = float(os.environ.get(TIMEOUT_ENV) or DEFAULT_TIMEOUT_S)
    path = lease_path().with_suffix(".queue.json")
    deadline = clock() + timeout_s
    last_report = -REPORT_EVERY_S
    ticket = {"pid": os.getpid(), "created": child_registry.process_create_time(os.getpid()), "exclusive": exclusive}
    granted = False
    try:
        while True:
            with _queue_lock():
                queue = [t for t in (_read(path) or {}).get("tickets", []) if _holder_alive(t)]
                if ticket not in queue:
                    queue.append(ticket)
                records = _records()
                _remove_records([(p, rec) for p, rec in records
                                 if rec and p.exists() and not _holder_alive(rec)
                                 and not (_read(p) or {}).get("legacy_guard")])
                live = [(p, r) for p, r in records if _holder_alive(r)]
                foreign = scan() if queue[0] == ticket else []
                if foreign and kill_orphans:
                    for pid, _img in foreign:
                        killed, note = child_registry.kill_if_orphan(pid)
                        log(f"[host-lease] game PID {pid}: {note}")
                    foreign = scan()
                managed = [r for _, r in live]
                foreign = [(pid, img) for pid, img in foreign if not _managed_engine(pid, managed)]
                # Slot IDs keep two-core spacing even for a narrower caller. Check
                # recorded masks as well, including leases from the old layout.
                occupied = [r.get("affinity_mask", ((1 << (2 * r.get("cores_per_slot", 1))) - 1)
                                  << (4 + r.get("slot", 0) * 2 * r.get("cores_per_slot", 1))) for _, r in live]
                free = [i for i in range(slot_count())
                        if (not slot_path(i).exists() or (_read(slot_path(i)) or {}).get("legacy_guard"))
                        and not any(affinity_mask(i) & mask for mask in occupied)]
                legacy = any("slot" not in rec for _, rec in live) or (lease_path().exists() and _read(lease_path()) is None)
                eligible = queue[0] == ticket and not foreign and not legacy and (not live if exclusive else
                            bool(free) and not any(r.get("exclusive") for _, r in live))
                if eligible:
                    allocated = []
                    for i in (range(slot_count()) if exclusive else free[:1]):
                        guard = i == 0 and (_read(slot_path(i)) or {}).get("legacy_guard")
                        taken, _ = (True, None) if guard else _try_take(slot_path(i), label)
                        if not taken:
                            _remove_records(allocated)
                            eligible = False
                            break
                        rec = {"pid": os.getpid(), "created": ticket["created"], "label": label,
                               "cwd": os.getcwd(), "argv": sys.argv[:3], "since": time.time()} if guard else _read(slot_path(i))
                        _write_record(slot_path(i), dict(rec, slot=i, exclusive=exclusive,
                                                       cores_per_slot=cores_per_slot(),
                                                       affinity_mask=0xFFF0 if exclusive else affinity_mask(i)))
                        allocated.append((slot_path(i), _read(slot_path(i))))
                    if eligible:
                        queue.pop(0)
                _write_record(path, {"tickets": queue})
                if eligible:
                    granted = True
                    if not _registered:
                        atexit.register(release)
                        _registered = True
                    return
                why = ("a game this process did not start is running: " + ", ".join(f"{img} pid {pid}" for pid, img in foreign)
                       if foreign else "the host game lease is held by " + _describe(live[0][1] if live else None))
            if not wait:
                raise HostLeaseBusy(why + "; queued behind another lane")
            if clock() >= deadline:
                raise HostLeaseBusy(f"gave up after {timeout_s:.0f} s: {why}")
            if clock() - last_report >= REPORT_EVERY_S:
                last_report = clock()
                log(bounded_wait.countdown_line("host game lease", why, deadline - clock(), timeout_s))
            sleep(POLL_S)
    finally:
        with _queue_lock():
            if not granted:
                _remove_records([(p, r) for p, r in _records() if r and r.get("pid") == os.getpid()])
            queue = (_read(path) or {}).get("tickets", [])
            _write_record(path, {"tickets": [t for t in queue if t != ticket and _holder_alive(t)]})


@contextmanager
def shared_window():
    """A launch under a normal slot: nothing is held outside the body, and a slot an engine launch took inside it is released at the end."""
    release()
    try:
        yield
    finally:
        release()


@contextmanager
def exclusive_window(label: str, **kwargs):
    """Hold every slot only for the body (one measured window), then give them back so other lanes' normal runs interleave.

    A held normal slot cannot be upgraded, so call this from a process that holds nothing outside the window."""
    release()
    acquire(label, exclusive=True, **kwargs)
    try:
        yield
    finally:
        release()


def _managed_engine(pid, holders):
    rec = child_registry.read_record(pid)
    if not rec or rec.get("create_time") != child_registry.process_create_time(pid):
        return False
    identity = rec.get("lease_holder")
    return any((identity == {key: holder.get(key) for key in ("pid", "created")} if identity else
                rec.get("runner_pid") == holder["pid"]) for holder in holders)


def ensure_held(label: str) -> None:
    """Hold the lease before an engine launch (re-entrant)."""
    acquire(label)


# ---------------------------------------------------------------- batch lease (hold / release)
# WHY: a proof chain (deploy check, smoke, capture, determinism pair) is several runner processes; between two of them another lane
# could take the lease and interleave its engine with the chain. `hold` parks the lease in a small holder process of its own (an agent's
# shell does not outlive one tool call, so no caller pid would do); runner processes started with MC2_HOST_LEASE_BATCH=<tag> enter it.
# The lease still ends on a dead holder pid (the usual rule), on `release`, and at the TTL, so a forgotten hold cannot wedge the host.

def _retry_file_change(operation, *args, **kwargs):
    """Windows readers can briefly deny replacement or deletion of a lease record."""
    deadline = time.monotonic() + 2.0
    while True:
        try:
            return operation(*args, **kwargs)
        except PermissionError as exc:
            remaining = deadline - time.monotonic()
            if getattr(exc, "winerror", None) not in (5, 32) or remaining <= 0:
                raise
            time.sleep(min(0.02, remaining))


def _write_record(path: Path, rec: dict) -> None:
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(rec), encoding="utf-8")
    _retry_file_change(os.replace, tmp, path)


def holder_loop(tag: str, ttl_s: float, *, exclusive: Optional[bool] = None, log: Callable[[str], None] = _log_default, sleep: Callable[[float], None] = time.sleep,
                clock: Callable[[], float] = time.time) -> int:
    """Body of the holder process: take the lease (waiting as usual), mark it as batch `tag`, keep it until it is released, taken
    away, or the TTL passes. -> 0 on a normal end, 4 when the lease could not be taken."""
    try:
        acquire(f"batch {tag}", log=log, exclusive=exclusive)
    except HostLeaseBusy as exc:
        log(f"[host-lease] batch {tag}: {exc}")
        return 4
    with _queue_lock():
        paths = [p for p, r in _records() if _ours(r)]
        for path in paths:
            rec = dict(_read(path) or {}, batch=tag, expires=clock() + ttl_s, label=f"batch {tag}")
            _write_record(path, rec)
    path = paths[0]
    while True:
        cur = _read(path)
        if not cur or cur.get("pid") != os.getpid() or cur.get("batch") != tag:
            return 0                                   # released (or cleared) from outside
        if clock() >= float(cur.get("expires", 0)):
            log(f"[host-lease] batch {tag}: TTL reached, releasing")
            release()
            return 0
        sleep(POLL_S)


def hold(tag: str, ttl_s: float = DEFAULT_BATCH_TTL_S, *, timeout_s: Optional[float] = None, exclusive: Optional[bool] = None,
         log: Callable[[str], None] = _log_default) -> int:
    """Start the holder process and wait until it holds the lease. -> its pid. Raises HostLeaseBusy when it cannot."""
    import subprocess
    try:
        from scripts import console_child         # type: ignore
    except ImportError:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import console_child                       # type: ignore
    if not tag or not all(c.isalnum() or c in "-_." for c in tag):
        raise ValueError(f"batch tag {tag!r}: use letters, digits, '-', '_' or '.'")
    ttl_s = min(max(ttl_s, 1.0), MAX_BATCH_TTL_S)
    flags = 0x00000200 | 0x01000000 if sys.platform == "win32" else 0     # new process group, break away from the caller's job
    argv = [sys.executable, str(Path(__file__).resolve()), "_holder", "--tag", tag, "--ttl", str(ttl_s)]
    if _exclusive(exclusive):
        argv.append("--exclusive")
    quiet = {"stdin": subprocess.DEVNULL, "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    try:
        proc = subprocess.Popen(argv, **console_child.child_kwargs(redirected=True, creationflags=flags, **quiet))
    except OSError:                                # the caller's job forbids breakaway: the holder then lives as long as that job
        proc = subprocess.Popen(argv, **console_child.child_kwargs(redirected=True, creationflags=flags & ~0x01000000, **quiet))
    deadline = time.monotonic() + (timeout_s if timeout_s is not None else float(os.environ.get(TIMEOUT_ENV) or DEFAULT_TIMEOUT_S))
    last = time.monotonic() - REPORT_EVERY_S + 2.0      # first line only after the holder had time to take a free lease
    while True:
        rec = next((r for _, r in _records() if r and r.get("pid") == proc.pid), None)
        if rec and rec.get("pid") == proc.pid and rec.get("batch") == tag:
            return proc.pid
        if proc.poll() is not None:
            raise HostLeaseBusy(f"batch {tag}: the holder exited (rc {proc.returncode}) without the lease")
        if time.monotonic() >= deadline:
            proc.terminate()
            raise HostLeaseBusy(f"batch {tag}: not held after the wait; the lease is held by {_describe(rec)}")
        if time.monotonic() - last >= REPORT_EVERY_S:
            last = time.monotonic()
            log(f"[host-lease] batch {tag}: waiting for the lease (held by {_describe(rec)})")
        time.sleep(0.25)


def release_batch(tag: str) -> Tuple[bool, Optional[dict]]:
    """End batch `tag`: remove its lease (the holder sees that and exits). -> (removed, the record). Another lease is left alone."""
    with _queue_lock():
        records = _records()
        matched = [(p, r) for p, r in records if r and r.get("batch") == tag]
        _remove_records(matched)
        return bool(matched), matched[0][1] if matched else records[0][1]


# ---------------------------------------------------------------- status / clear-dead

def status() -> dict:
    """What the lease looks like right now: the file, its holder (and whether that process is alive), the engines running."""
    path = lease_path()
    records = [r for _, r in _records() if r]
    rec = next((r for r in records if _holder_alive(r)), records[0] if records else None)
    engines = [{"pid": pid, "image": img, "path": child_registry.image_path(pid)} for pid, img in foreign_engines()]
    return {"path": str(path), "holder": rec, "holder_alive": _holder_alive(rec) if rec else False,
            "slots": records, "engines": engines, "off": disabled(lambda m: None), "private": redirected()}


def clear_dead() -> Tuple[bool, Optional[dict]]:
    """Remove a lease whose holder process is gone. -> (removed, the record). A live holder is left alone: that lane is running."""
    with _queue_lock():
        removed = None
        records = _records()
        for path, rec in records:
            if path.exists() and not _holder_alive(rec):
                removed = rec
        _remove_records([(p, r) for p, r in records if r and p.exists() and not _holder_alive(r)
                         and not (_read(p) or {}).get("legacy_guard")])
        return removed is not None, removed or next((r for _, r in records if r), None)


def _print_status(st: dict) -> int:
    print(f"host lease file: {st['path']}" + (" (private, test)" if st["private"] else "") + (" [OFF: owner]" if st["off"] else ""))
    rec = st["holder"]
    if rec is None:
        print("holder: none (free)")
        code = 0
    else:
        since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(rec.get("since", 0))))
        print(f"holder: {_describe(rec)} since {since}, process {'ALIVE' if st['holder_alive'] else 'DEAD (clear-dead removes it)'}")
        code = 1 if st["holder_alive"] else 2
    for e in st["engines"]:
        print(f"engine: {e['image']} pid {e['pid']} {e['path'] or ''}")
    if not st["engines"]:
        print("engines running: none")
    return code


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="host_lease", description="the host-wide engine lease")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="holder, whether it is alive, engines running (exit 0 free / 1 held / 2 held by a dead process)")
    sub.add_parser("clear-dead", help="remove the lease when its holder process is dead; a live holder is left alone")
    h = sub.add_parser("hold", help="hold the lease for a chain of runs: start a holder process, print the env line the chain's runs need")
    h.add_argument("--tag", required=True, help="batch name; runs started with MC2_HOST_LEASE_BATCH=<tag> enter the held lease")
    h.add_argument("--exclusive", action="store_true", default=None)
    h.add_argument("--ttl", type=float, default=DEFAULT_BATCH_TTL_S, help=f"seconds until the lease lapses (max {MAX_BATCH_TTL_S:.0f})")
    h.add_argument("--wait-timeout", type=float, default=None, help="seconds to wait for the lease (default MC2_HOST_LEASE_TIMEOUT or 3600)")
    r = sub.add_parser("release", help="end a batch lease taken with hold")
    r.add_argument("--tag", required=True)
    hl = sub.add_parser("_holder")                 # the holder process itself; started by hold
    hl.add_argument("--tag", required=True)
    hl.add_argument("--exclusive", action="store_true", default=None)
    hl.add_argument("--ttl", type=float, required=True)
    argv = ["clear-dead" if a == "--clear-dead" else a for a in (sys.argv[1:] if argv is None else argv)]   # deploy_lease spelling
    args = ap.parse_args(argv)
    if args.cmd == "status":
        return _print_status(status())
    if args.cmd == "_holder":
        return holder_loop(args.tag, args.ttl, exclusive=args.exclusive)
    if args.cmd == "hold":
        try:
            pid = hold(args.tag, args.ttl, timeout_s=args.wait_timeout, exclusive=args.exclusive)
        except (HostLeaseBusy, ValueError) as exc:
            print(f"[host-lease] {exc}", file=sys.stderr)
            return 4
        print(f"[host-lease] batch {args.tag} held by pid {pid} for at most {min(args.ttl, MAX_BATCH_TTL_S):.0f} s; "
              f"run the chain with {BATCH_ENV}={args.tag}, end it with: host_lease release --tag {args.tag}")
        return 0
    if args.cmd == "release":
        removed, rec = release_batch(args.tag)
        print(f"[host-lease] batch {args.tag} released" if removed else
              f"[host-lease] batch {args.tag} not held (lease: {_describe(rec) if rec else 'free'})")
        return 0 if removed else 1
    removed, rec = clear_dead()
    if removed:
        print(f"[host-lease] cleared the lease left by {_describe(rec)}")
    elif rec is None:
        print("[host-lease] no lease to clear")
    else:
        print(f"[host-lease] NOT cleared: {_describe(rec)} is alive; wait for that run")
    return 0 if removed or rec is None else 1


if __name__ == "__main__":
    sys.exit(main())

"""Host-wide lint worker semaphore and forced scratch cleanup, split out of lane_merge.py (file-size ratchet)."""
import os
import re
import shutil
import tempfile
import contextlib
import time
from pathlib import Path


class LintSlots:
    """Host-wide counting semaphore across processes: N slot files, each held by an exclusive OS byte lock (the OS drops
    it when the holder dies). Caps concurrent lint workers on the machine at JOBS however many `--lint` processes run."""
    def __init__(self, directory=None, n=None, sleep=time.sleep):
        ops = "A:/Games/mc2-ops"
        self.dir = directory or os.environ.get("MC2_LINT_SLOTS_DIR") or (
            os.path.join(ops, "lint-slots") if os.path.isdir(ops) else os.path.join(tempfile.gettempdir(), "mc2-lint-slots"))
        self.n, self.sleep = n or self._jobs(), sleep

    @staticmethod
    def _jobs():
        try:
            return max(1, int(Path("A:/Games/mc2-ops/JOBS").read_text().strip()))
        except (OSError, ValueError):
            if os.name == "nt":
                return min(6, os.cpu_count() or 2)
            cpus = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 2)
            return max(2, min(16, cpus // 2))   # Linux (the box: 32 build cores): half the usable cores, capped

    @staticmethod
    def _lock(fh):
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def try_acquire(self):
        """The open handle of a free slot (keeping it open holds the slot), or None when all N are busy."""
        os.makedirs(self.dir, exist_ok=True)
        for i in range(self.n):
            fh = open(os.path.join(self.dir, f"slot{i}.lock"), "a+")
            try:
                self._lock(fh)
                return fh
            except OSError:
                fh.close()
        return None

    def _take_ticket(self):
        """Join the FIFO: a ticket file named by start time, held under an OS lock while we wait (dead waiters' tickets are unlocked)."""
        qdir = os.path.join(self.dir, "queue")
        os.makedirs(qdir, exist_ok=True)
        path = os.path.join(qdir, f"{time.time_ns():020d}-{os.getpid()}-{id(self) & 0xFFFF:04x}.q")
        fh = open(path, "a+")
        self._lock(fh)
        return fh, path

    def _is_head(self, own_path):
        """True when no earlier live ticket exists. An earlier ticket whose lock we can take belongs to a dead waiter: remove it."""
        qdir = os.path.dirname(own_path)
        for name in sorted(os.listdir(qdir)):
            path = os.path.join(qdir, name)
            if path == own_path or name > os.path.basename(own_path):
                return True
            try:
                with open(path, "a+") as other:
                    try:
                        self._lock(other)
                    except OSError:
                        return False      # a live waiter started earlier
                    young = time.time() - os.path.getmtime(path) < 2   # created but not yet locked by its owner
                if young:
                    return False
                with contextlib.suppress(OSError):
                    os.remove(path)
            except OSError:
                continue
        return True

    def acquire(self, wait_s=7200):
        """Grant slots in start order: take a ticket, wait until no earlier live ticket remains, then take a free slot."""
        deadline = time.monotonic() + wait_s
        announced = False
        ticket, tpath = self._take_ticket()
        try:
            while True:
                if self._is_head(tpath):
                    fh = self.try_acquire()
                    if fh:
                        return fh
                if time.monotonic() >= deadline:
                    raise TimeoutError("no lint slot free in " + self.dir)
                if not announced:
                    print(f"lint: waiting for lint slot ({self.n} busy)", flush=True)
                    announced = True
                self.sleep(2)
        finally:
            ticket.close()
            with contextlib.suppress(OSError):
                os.remove(tpath)

    def run(self, fn, *args):
        fh = self.acquire()
        try:
            return fn(*args)
        finally:
            fh.close()   # closing drops the OS lock


def rmtree_force(path):
    """Remove a tree including read-only git objects (Windows); never raises."""
    def _onerr(func, p, _exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except OSError:
            pass
    shutil.rmtree(path, onerror=_onerr)


@contextlib.contextmanager
def scratch_dir(prefix, parent=None):
    """mkdtemp that is always removed on exit, success or failure, even with read-only git objects inside."""
    path = tempfile.mkdtemp(prefix=prefix, dir=parent)
    try:
        yield path
    finally:
        rmtree_force(path)


SCRATCH_NAME = re.compile(r"(lane-lint-|stale-inv-)[A-Za-z0-9_]{8}")   # exactly what tempfile.mkdtemp(prefix=...) produces


def _newest_mtime(path):
    newest = os.path.getmtime(path)
    with contextlib.suppress(OSError):
        for entry in os.scandir(path):
            newest = max(newest, entry.stat().st_mtime)
    return newest


def sweep_stale_scratch(parents, max_age_s=7200, now=None):
    """Remove lane-lint-*/stale-inv-* scratch dirs left by dead lints: each matched by exact mkdtemp name, older than
    max_age_s (newest of the dir and its top-level entries), and not held by a live process (a rename of a dir that any
    process has open or as its cwd fails on Windows, which is the liveness test). Returns the removed paths."""
    now, removed = now or time.time(), []
    for parent in parents:
        try:
            names = os.listdir(parent) if parent else []
        except OSError:
            continue
        for name in names:
            path = os.path.join(parent, name)
            try:
                if not SCRATCH_NAME.fullmatch(name) or not os.path.isdir(path) or os.path.islink(path):
                    continue
                if now - _newest_mtime(path) < max_age_s:
                    continue
                parked = path + ".sweeping"
                os.rename(path, parked)   # fails while a live process holds the tree
            except OSError:
                continue
            rmtree_force(parked)
            removed.append(path)
    return removed


CPU_SPLIT_DEFAULT = {"game0": 0x0003, "game1": 0x000C, "compile": 0xFFF0}   # 5800X3D, 16 logical CPUs, SMT siblings adjacent: one core per lane game, the other 6 cores (12 CPUs) for compiles


def cpu_masks():
    """Masks from A:/Games/mc2-ops/CPU_SPLIT (lines game0=, game1=, compile=; the file is the one source of truth, scripts/smoke_lib/host_lease.py reads the same).
    Derived modes: "lane" = compile (lane lint/build work never touches a game core); "gate" = every CPU (the rare PC gate fallback holds both game slots)."""
    masks = dict(CPU_SPLIT_DEFAULT)
    try:
        for line in Path("A:/Games/mc2-ops/CPU_SPLIT").read_text().splitlines():
            k, _, v = line.partition("=")
            if k.strip() in masks and v.strip():
                masks[k.strip()] = int(v.strip(), 0)
    except (OSError, ValueError):
        pass
    masks["lane"] = masks["compile"]
    masks["gate"] = masks["game0"] | masks["game1"] | masks["compile"]
    return masks


def pin_cpus(mode):
    """Pin this process to the `mode` mask ("lane"/"compile" = the 6 compile cores, "gate" = all); children inherit the affinity. Returns the applied mask (0 = not applied)."""
    mask = cpu_masks()[mode]
    if os.environ.get("MC2_NO_CPU_PIN") or (os.name != "nt" and not os.path.exists("A:/Games/mc2-ops/CPU_SPLIT")):   # the PC's 16-CPU split means nothing elsewhere; the box pins with taskset   # pytest: never pin the test runner
        return 0
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        k32.GetCurrentProcess.restype = ctypes.c_void_p
        h = ctypes.c_void_p(k32.GetCurrentProcess())
        proc, sysm = ctypes.c_size_t(), ctypes.c_size_t()
        k32.GetProcessAffinityMask(h, ctypes.byref(proc), ctypes.byref(sysm))
        mask &= sysm.value
        if not mask or not k32.SetProcessAffinityMask(h, ctypes.c_size_t(mask)):
            return 0
    elif hasattr(os, "sched_setaffinity"):
        cpus = {i for i in range(mask.bit_length()) if mask >> i & 1} & os.sched_getaffinity(0)
        if not cpus:
            return 0
        os.sched_setaffinity(0, cpus)
    else:
        return 0
    print(f"cpu split: {mode} affinity mask=0x{mask:04X}", flush=True)
    return mask

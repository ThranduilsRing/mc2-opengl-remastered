"""Host-wide lint worker semaphore and forced scratch cleanup, split out of lane_merge.py (file-size ratchet)."""
import os
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
            return min(6, os.cpu_count() or 2)

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

    def acquire(self, wait_s=7200):
        deadline = time.monotonic() + wait_s
        announced = False
        while True:
            fh = self.try_acquire()
            if fh:
                return fh
            if time.monotonic() >= deadline:
                raise TimeoutError("no lint slot free in " + self.dir)
            if not announced:
                print(f"lint: waiting for lint slot ({self.n} busy)", flush=True)
                announced = True
            self.sleep(2)

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


CPU_SPLIT_DEFAULT = {"gate": 0x00FF, "lane": 0xFF00}   # Ryzen 7 5800X3D, 16 logical CPUs: merge gate on 0-7, lane_merge lane work on 8-15


def cpu_masks():
    """{"gate": mask, "lane": mask} from A:/Games/mc2-ops/CPU_SPLIT (lines `gate=0x00FF`, `lane=0xFF00`), else the defaults."""
    masks = dict(CPU_SPLIT_DEFAULT)
    try:
        for line in Path("A:/Games/mc2-ops/CPU_SPLIT").read_text().splitlines():
            k, _, v = line.partition("=")
            if k.strip() in masks and v.strip():
                masks[k.strip()] = int(v.strip(), 0)
    except (OSError, ValueError):
        pass
    return masks


def pin_cpus(mode):
    """Pin this process to the `mode` half ("gate" or "lane"); children inherit the affinity. Returns the applied mask (0 = not applied)."""
    mask = cpu_masks()[mode]
    if os.environ.get("MC2_NO_CPU_PIN"):   # pytest: never pin the test runner
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

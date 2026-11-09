# scripts/smoke_lib/host_lease.py
"""FIFO engine slots shared across worktrees, with exclusive reservations for timing proofs.

MC2_HOST_CORES_PER_SLOT selects physical cores per slot (default 2).
Slot count has ONE source for every worktree: 3 while the owner cores 0-1 waiver file (WAIVER_PATH, holds an expiry epoch)
is live, otherwise 2. MC2_HOST_GAME_SLOTS is honoured only under a private test/farm lease file. Cores 0..1 stay reserved.
Queue order: merge-gate stages (MC2_IN_GATE=1) and exclusive holds go ahead of pass/fail runs; FIFO within a class.
While a full gate runs (gate window marker, see open_gate_window) pass/fail runs also wait out the gaps between its stages.
MC2_HOST_LEASE_EXCLUSIVE=1 reserves every slot. Batch hold/release keeps a reservation
across a serial chain. Dead holders and tickets recover automatically. Private paths
isolate tests; disabling leases requires MC2_OWNER_NO_LEASE=1.

LOCK ORDER (every launcher): host game SLOT first, then the install's deploy-FOLDER lease (smoke_lib/deploy_lease.py).
A process that holds a folder lease never waits for a slot: acquire() raises LockOrderError instead (W114: a headless
det run held the candidate install's folder and waited for a slot while tier1 held a slot and waited for that folder;
only the 3600 s bound broke it). --headless runs take no slot at all (run_smoke and nofocus both skip it). A slot held
through a batch (MC2_HOST_LEASE_BATCH) counts as taken first. Inside the gate a folder wait whose holder is itself
queued for a slot fails within deploy_lease.BLOCKED_HOLDER_S and names both sides.
"""
from __future__ import annotations

import argparse
import atexit
import json
import os
import re
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
WAIVER_PATH = Path("A:/Games/mc2-ops/owner_cores_waived_until")
POLL_S = 2.0
REPORT_EVERY_S = 30.0

IDLE_RECLAIM_S = float(os.environ.get("MC2_HOST_LEASE_IDLE_S") or 900)   # holder alive but no engine on the host this long -> reclaim
MERGE_IDLE_TTL_S = 1200.0                # merge holders get at most 20 idle minutes


def cores_per_slot() -> int:
    width = int(os.environ.get("MC2_HOST_CORES_PER_SLOT", "2"))
    if not 1 <= width <= 6:
        raise ValueError("MC2_HOST_CORES_PER_SLOT must be between 1 and 6")
    return width


def slot_count() -> int:
    maximum = 6 // max(2, cores_per_slot())
    if not redirected():
        # One shared answer for every worktree, so two processes can never disagree and over-admit: one slot per lane GPU.
        return len(lane_devices())
    count = int(os.environ.get("MC2_HOST_GAME_SLOTS", "2"))
    if not 1 <= count <= maximum:
        raise ValueError(f"MC2_HOST_GAME_SLOTS must be between 1 and {maximum}")
    return count


def waiver_active(now: Optional[float] = None) -> bool:
    """True while the owner cores 0-1 waiver file exists and its expiry epoch is in the future (unparseable = inactive)."""
    try:
        text = WAIVER_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        return False
    try:
        return float(text) > (time.time() if now is None else now)
    except ValueError:
        return False   # an empty or half-written waiver must never raise the slot count


GPU_SPLIT_PATH = Path("A:/Games/mc2-ops/GPU_SPLIT")   # lines `slot0=AMD`, `slot1=NVIDIA` (device-name substrings, like MC2_VK_DEVICE)
GPU_SPLIT_DEFAULT = ("AMD", "NVIDIA")                  # slot 0 = RX 7900 XTX, slot 1 = RTX 2070 SUPER: both are LANE devices (no gate device on the PC)
DEVICE_ALIASES = {"AMD": ("amd", "radeon", "xtx", "7900"), "NVIDIA": ("nvidia", "geforce", "rtx", "2070")}


LEGACY_GPU_KEYS = {"lane": 0, "gate": 1}   # old GPU_SPLIT keys (lane=AMD, gate=NVIDIA), accepted for one window: lane = slot 0, gate = slot 1
GPU_SPLIT_KEYS = frozenset({"slot0", "slot1", "gate_aux", *LEGACY_GPU_KEYS})   # gate_aux: merge_gate.aux_device()


def _parse_gpu_split(text: str) -> tuple:
    devices = list(GPU_SPLIT_DEFAULT)
    for n, line in enumerate(text.splitlines(), 1):   # p31: an unknown key (slot2=, Slot0=) silently kept the default device
        k, eq, _ = line.partition("=")
        if line.strip() and not line.lstrip().startswith("#") and (not eq or k.strip() not in GPU_SPLIT_KEYS):
            raise ValueError(f"GPU_SPLIT line {n}: unknown key {k.strip()!r} (known: {', '.join(sorted(GPU_SPLIT_KEYS))})")
    for legacy_pass in (True, False):   # legacy keys first so an explicit slotN line always wins
        for line in text.splitlines():
            k, _, v = line.partition("=")
            k = k.strip()
            m = re.fullmatch(r"slot([01])", k)
            index = LEGACY_GPU_KEYS.get(k) if legacy_pass else (int(m.group(1)) if m else None)
            if index is not None and v.strip():
                devices[index] = v.strip()
    return tuple(devices)


def lane_devices() -> tuple:
    """The two lane GPUs by device-name substring; lease slot i drives device i. The PC has no gate GPU."""
    if redirected():
        return GPU_SPLIT_DEFAULT
    try:
        return _parse_gpu_split(GPU_SPLIT_PATH.read_text(encoding="utf-8"))
    except OSError:
        return GPU_SPLIT_DEFAULT


_HOST_GPU_NAMES: Optional[List[str]] = None


def host_gpu_names() -> Optional[List[str]]:
    """Lower-case adapter names of THIS host (Win32_VideoController), cached; None when it cannot be read or a test redirects the lease."""
    global _HOST_GPU_NAMES
    if redirected():
        return None
    if _HOST_GPU_NAMES is None:
        try:
            from smoke_lib import crash_evidence
            info = crash_evidence._gpu_info(20)
            rows = info if isinstance(info, list) else [info] if isinstance(info, dict) else []
            _HOST_GPU_NAMES = [str(r.get("Name", "")).lower() for r in rows if isinstance(r, dict) and r.get("Name")]
        except Exception:
            _HOST_GPU_NAMES = []
    return _HOST_GPU_NAMES or None


def device_on_host(label: str, names: Optional[List[str]] = None) -> Optional[bool]:
    """Does a GPU matching the MC2_VK_DEVICE substring `label` exist on this host? None = unknown (do not second-guess)."""
    names = host_gpu_names() if names is None else names
    if not names:
        return None
    tokens = (label.lower(),) + tuple(DEVICE_ALIASES.get(label, ()))
    return any(t in n for n in names for t in tokens)


def host_lane_device(devices: Optional[tuple] = None, names: Optional[List[str]] = None) -> Optional[str]:
    """The first lane GPU (GPU_SPLIT order) this host really has: the PC gives slot 0 (the XTX), the GTX 1050 Ti laptop gives
    slot 1 (NVIDIA). None when the host has none of them: force nothing and let the engine and run_smoke route. Unknown host = slot 0."""
    devices = lane_devices() if devices is None else devices
    names = host_gpu_names() if names is None else names
    if not names:
        return devices[0]
    return next((d for d in devices if device_on_host(d, names)), None)


def slot_device(slot: int) -> str:
    devices = lane_devices()
    return devices[slot % len(devices)]


def device_hint(env=None) -> Optional[str]:
    """The MC2_VK_DEVICE substring a run FORCES: explicit MC2_VK_DEVICE or MC2_HARNESS_GPU=2070|xtx. None = the lease slot it
    wins decides (FIFO across both lane GPUs), which includes MC2_HARNESS_GPU=any."""
    env = os.environ if env is None else env
    mode = (env.get("MC2_HARNESS_GPU") or "").lower()
    return env.get("MC2_VK_DEVICE") or {"2070": "NVIDIA", "xtx": "AMD"}.get(mode) or None


def _device_matches(slot: int, forced: str) -> bool:
    label, low = slot_device(slot), forced.lower()
    return low in label.lower() or label.lower() in low or any(t in low for t in DEVICE_ALIASES.get(label, ()))


def eligible_slots(forced: Optional[str]) -> List[int]:
    """Slots a run may take: all of them, or only those whose device matches a forced device. A forced device that names no
    lane GPU at all (a farm laptop, a typo) is not filtered: the engine reports 'matches no accepted device'."""
    every = list(range(slot_count()))
    if not forced:
        return every
    return [i for i in every if _device_matches(i, forced)] or every


@contextmanager
def keyed_by(device: Optional[str]):
    """Run lease calls keyed to `device` (the MC2_VK_DEVICE a child is about to get), so the lease, the queue and the core
    reservation follow the env the child actually receives and not this process's own. None = no change."""
    if not device:
        yield
        return
    prev = os.environ.get("MC2_VK_DEVICE")
    os.environ["MC2_VK_DEVICE"] = device
    try:
        yield
    finally:
        if prev is None:
            os.environ.pop("MC2_VK_DEVICE", None)
        else:
            os.environ["MC2_VK_DEVICE"] = prev


def slot_path(slot: int, path: Optional[Path] = None) -> Path:
    path = path or lease_path()
    return path if slot == 0 else path.with_name(path.stem + f".slot{slot}" + path.suffix)


def affinity_mask(slot: int) -> int:
    """One physical core (two SMT threads) per lane game, a different core per GPU slot (CPU_SPLIT game0/game1)."""
    width = cores_per_slot()
    if not 0 <= slot < 6 // max(2, width):
        raise ValueError("slot exceeds the physical game core budget")
    # The CPU_SPLIT masks describe the owner's PC only (W130 review): the laptop gate sets MC2_NO_CPU_PIN and keeps the legacy shape.
    if not redirected() and sys.platform == 'win32' and not os.environ.get("MC2_NO_CPU_PIN"):
        return game_masks()[slot % 2]
    return ((1 << (width * 2)) - 1) << (4 + slot * max(2, width) * 2)


CPU_SPLIT_PATH = Path("A:/Games/mc2-ops/CPU_SPLIT")
CPU_SPLIT_DEFAULT = {"game0": 0x0003, "game1": 0x000C, "compile": 0xFFF0}   # 5800X3D: SMT siblings are adjacent (2k, 2k+1); 2 cores for games, 6 for compiles


def _cpu_split() -> dict:
    """{"game0", "game1", "compile"} masks from A:/Games/mc2-ops/CPU_SPLIT (same file and defaults as tools/lint_slots.cpu_masks)."""
    try:
        return parse_cpu_split(CPU_SPLIT_PATH.read_text())
    except OSError:
        return dict(CPU_SPLIT_DEFAULT)


def parse_cpu_split(text: str) -> dict:
    """Same grammar as tools/lint_slots.parse_cpu_split (a test holds them equal). The old `lane=` mask is accepted for one window
    as `compile` when the file has no compile line; the old `gate=` mask has no meaning any more and is ignored. An unknown key or a bad value raises ValueError."""
    masks, legacy_lane = dict(CPU_SPLIT_DEFAULT), None
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        k, eq, v = line.partition("=")
        k, v = k.strip(), v.strip()
        if not eq or k not in (*masks, "lane", "gate"):   # p31: a typo (gameO=, complie=) silently kept the default mask
            raise ValueError(f"CPU_SPLIT line {n}: unknown key {k!r} (known: game0, game1, compile; legacy lane, gate)")
        try:
            value = int(v, 0)
        except ValueError:
            raise ValueError(f"CPU_SPLIT line {n}: {k}={v!r} is not an integer mask") from None
        if k in masks:
            masks[k] = value
        elif k == "lane":
            legacy_lane = value
    if legacy_lane and not re.search(r"^\s*compile\s*=\s*\S", text, re.M):
        masks["compile"] = legacy_lane
    return masks


def game_masks() -> List[int]:
    masks = _cpu_split()
    return [masks["game0"], masks["game1"]]


def game_cores():
    """The logical CPUs the lane games may use (the union of both per-game cores); every compile runs on the rest (CPU_SPLIT compile)."""
    union = game_masks()[0] | game_masks()[1]
    return [c for c in range(union.bit_length()) if union >> c & 1]


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


def _game_mask(records, ours) -> int:
    """Borrow both game cores only while no other live lease holds one."""
    mask = 0
    for rec in ours:
        mask |= rec.get("affinity_mask", 3 << (4 + rec.get("slot", 0) * 2))
    if not redirected() and sys.platform == 'win32':
        reserved = sum(1 << c for c in game_cores())
        mask = ((mask & reserved) or reserved) if any(r not in ours for r in records) else reserved
    return mask


def _set_affinity(handle, pid, mask) -> None:
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        kernel.SetProcessAffinityMask.restype = wintypes.BOOL
        if not kernel.SetProcessAffinityMask(handle, mask):
            raise ctypes.WinError(ctypes.get_last_error())
        kernel.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        kernel.GetProcessAffinityMask.restype = wintypes.BOOL
        actual, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(handle, ctypes.byref(actual), ctypes.byref(system)):
            raise ctypes.WinError(ctypes.get_last_error())
        if actual.value != mask:
            raise RuntimeError(f"engine affinity mismatch: requested {mask:#x}, applied {actual.value:#x}")
    elif hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(pid, {i for i in range(16) if mask & (1 << i)})
    priority = "ABOVE_NORMAL" if sys.platform == "win32" else "inherited"
    print(f"[host-affinity] pid={pid} mask={mask:#x} logical_cpus={mask.bit_count()} priority={priority}", file=sys.stderr, flush=True)


def _refresh_game_affinity() -> None:
    """Called under the queue lock: narrow a running sole game before admitting a neighbour."""
    if redirected() or sys.platform != "win32" or os.environ.get("MC2_NO_CPU_PIN"):
        return
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    live = [(p, r) for p, r in _records() if _holder_alive(r)]
    for path, rec in live:
        mask = _game_mask([r for _, r in live], [rec])
        for child in rec.get("engines", []):
            pid = child["pid"]
            if child.get("created") is None or child_registry.process_create_time(pid) != child["created"]:
                continue
            handle = kernel.OpenProcess(0x1200, False, pid)  # SET_INFORMATION | QUERY_LIMITED_INFORMATION
            if not handle:
                if child_registry.pid_alive(pid):
                    raise ctypes.WinError(ctypes.get_last_error())
                continue
            try:
                # Recheck after opening: never change a reused PID's affinity.
                if child_registry.process_create_time(pid) == child["created"]:
                    _set_affinity(handle, pid, mask)
            finally:
                kernel.CloseHandle(handle)


def apply_affinity(proc) -> None:
    """Pin an engine or its launcher and remember its identity for lease transitions."""
    if os.environ.get("MC2_NO_CPU_PIN"):
        print(f"[host-affinity] pid={proc.pid} mask=unrestricted priority=ABOVE_NORMAL pin=off", file=sys.stderr, flush=True)
        return
    with _queue_lock():
        live = [(p, r) for p, r in _records() if _holder_alive(r)]
        ours = [(p, r) for p, r in live if _ours(r)]
        if not ours:
            return
        try:
            _set_affinity(int(proc._handle) if sys.platform == "win32" else None, proc.pid,
                          _game_mask([r for _, r in live], [r for _, r in ours]))
            if not redirected() and sys.platform == "win32":
                path, rec = ours[0]
                children = [c for c in rec.get("engines", []) if child_registry.process_create_time(c["pid"]) == c.get("created")]
                children.append({"pid": proc.pid, "created": child_registry.process_create_time(proc.pid)})
                _write_record(path, dict(rec, engines=children))
        except BaseException:
            proc.kill()
            raise


class HostLeaseBusy(RuntimeError):
    """Another lane holds the host lease, or a game this process did not start is still running."""


class LockOrderError(RuntimeError):
    """This process asked for a host slot while it holds a deploy-folder lease (see LOCK ORDER above). Never retried."""


def _folders_held_here() -> List[str]:
    """Deploy-folder leases this process holds, read from the shared registry. A process can only hold one through
    deploy_lease.acquire_lease, so deploy_lease is already loaded when it matters; no import here (the batch holder runs
    this file as a script, where deploy_lease's package imports fail)."""
    held: List[str] = []
    for name in ("scripts.smoke_lib.deploy_lease", "smoke_lib.deploy_lease", "deploy_lease"):
        mod = sys.modules.get(name)
        if mod is not None and hasattr(mod, "held_by_pid"):
            held += [f for f in mod.held_by_pid(os.getpid()) if f not in held]
    return held


def waiting_ticket(pid: int) -> Optional[dict]:
    """The live slot-queue ticket of `pid` (it is waiting for a host slot), else None."""
    tickets = (_read(lease_path().with_suffix(".queue.json")) or {}).get("tickets", [])
    return next((t for t in tickets if t.get("pid") == pid and _holder_alive(t)), None)


def _log_default(m: str) -> None:
    print(m, file=sys.stderr, flush=True)


def redirected() -> bool:
    """True when a private lease file is in use (a test): the host's real engines are then not this process's business."""
    return bool(os.environ.get(PATH_ENV))


def lease_path() -> Path:
    """One lease file set for the whole PC: slot i = lane GPU i = game core i."""
    return _base_lease_path()


def _base_lease_path() -> Path:
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




QUIET_PATH = Path("A:/Games/mc2-ops/GATE_QUIET.json")
QUIET_OVERRIDE_ENV = "MC2_GATE_QUIET_OVERRIDE"   # "1" (set by an explicit --override-gate-quiet flag, owner only) lets one run through
QUIET_RC = 75                                     # exit code of a launcher that refused: distinct from lease-busy (4)


class GateQuiet(HostLeaseBusy):
    """A merge gate is running its full pass: lane game runs are refused outright, not queued (W114 lost 2 h to two 3600 s lease waits)."""
    rc = QUIET_RC


def gate_window_path() -> Path:
    return _base_lease_path().with_suffix(".gate.json") if redirected() else QUIET_PATH


def open_gate_window(head: str = "", window: str = "") -> None:
    """Called by the merge gate for its whole run: lane (non-gate) game runs are refused while this marker is live."""
    pid = os.getpid()
    held = _read(gate_window_path())
    if held and held.get("pid") != pid and _holder_alive(held):
        # Only the pid that wrote the marker owns it: a concurrent `merge_gate --publish` / `--drop-candidate` must not take over (and then delete) the running gate's marker.
        _log_default(f"[host-lease] gate quiet marker is held by live gate pid {held.get('pid')}; not overwriting it")
        return
    _write_record(gate_window_path(), {"pid": pid, "created": child_registry.process_create_time(pid), "started": time.time(),
                                       "head": head, "window": window, "cmdline": " ".join(sys.argv)[:300]})


def close_gate_window() -> None:
    rec = _read(gate_window_path())
    if rec and rec.get("pid") == os.getpid():
        try:
            gate_window_path().unlink()
        except OSError:
            pass


@contextmanager
def gate_capture_lease(head: str = "", window: str = ""):
    """Per-stage exclusive capture lease (p29 cut 1): the quiet marker exists only while a capture stage runs, never for the whole window.
    Lane game runs on the gate GPU queue on the normal lease the rest of the time; lanes on the other GPU were never affected."""
    open_gate_window(head, window)
    try:
        yield
    finally:
        close_gate_window()


def check_gate_quiet(log: Callable[[str], None] = _log_default) -> None:
    """Raise GateQuiet when a live gate (not this process, not one of its children) holds the quiet marker. A stale one is logged and ignored."""
    rec = _read(gate_window_path())
    if not rec or rec.get("pid") == os.getpid() or os.environ.get("MC2_IN_GATE") == "1" or os.getppid() == rec.get("pid"):
        return                                   # a run on the lane's GPU never waits for the gate
    if os.environ.get(QUIET_OVERRIDE_ENV) == "1":
        log(f"[host-lease] GATE QUIET overridden ({QUIET_OVERRIDE_ENV}=1): gate pid {rec.get('pid')}")
        return
    if not _holder_alive(rec):
        log(f"[host-lease] ignoring stale gate quiet marker (gate pid {rec.get('pid')} is dead)")
        return
    since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(rec.get("started", 0))))
    raise GateQuiet(f"GATE QUIET: the PC gate fallback holds both lane GPUs, refusing game run (gate pid {rec.get('pid')} since {since}, head {rec.get('head') or '?'}, "
                    f"window {rec.get('window') or '?'}); retry after the gate or pass the explicit override flag")


def gate_window_blocks(now: Optional[float] = None) -> bool:
    """True while a live gate (not this process) holds the window and the marker is inside its TTL."""
    rec = _read(gate_window_path())
    if not rec or rec.get("pid") == os.getpid() or not _holder_alive(rec) or os.environ.get(QUIET_OVERRIDE_ENV) == "1":
        return False
    return True                                  # pid alive (and same create time) is the only staleness test: a gate may run for hours


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
        _refresh_game_affinity()


_registered = False


def held_device() -> Optional[str]:
    """The lane GPU of the one slot this process (or its batch) holds, for MC2_VK_DEVICE; None for an exclusive hold or no hold."""
    records = [rec for _, rec in _records() if _ours(rec)]
    return records[0].get("device") if len(records) == 1 and not records[0].get("exclusive") else None


def holds() -> bool:
    """True when this process already holds the lease, itself or as a member of the live batch named by MC2_HOST_LEASE_BATCH."""
    return any(_ours(rec) for _, rec in _records())


def _batch_member(rec: Optional[dict]) -> bool:
    """True when `rec` is a batch lease whose tag this process carries in MC2_HOST_LEASE_BATCH."""
    tag = os.environ.get(BATCH_ENV, "")
    return bool(rec and tag and rec.get("batch") == tag)


def _describe(rec: Optional[dict]) -> str:
    if not rec:
        return "unknown holder (no record)"
    return (f"{rec.get('label', '?')} (pid {rec.get('pid')}, {' '.join(map(str, rec.get('argv', [])))}"
            f"{', in ' + str(rec['cwd']) if rec.get('cwd') else ''})")


def foreign_engines() -> List[Tuple[int, str]]:
    """Engines running on the host as (pid, image). Empty under a private lease file: a test never looks at the real host."""
    if redirected():
        return []
    return child_registry.engine_processes()


def _holder_tag(rec: Optional[dict]) -> str:
    """The batch/lane tag of a record: `merge-gate-123` for `batch merge-gate-123`, else the label as written."""
    tag = str(rec.get("batch") or rec.get("label") or "") if rec else ""
    return tag[len("batch "):] if tag.startswith("batch ") else tag


def _reclaim_idle(live, engines, log):
    """A live holder with no engine on the host for IDLE_RECLAIM_S is stuck (2026-10-27: one blocked lanes for hours): drop it.
    Batch holds have their own TTL; `merge*` holders get a bounded 20-minute idle allowance.
    First-seen-idle times live in a sidecar file (the slot records stay untouched)."""
    side = lease_path().with_suffix(".idle.json")
    now, seen, keep = time.time(), (_read(side) or {}), []
    marks = {}
    for p, rec in live:
        cur = _read(p)
        key = f"{rec.get('pid')}:{rec.get('created')}"
        if not cur or cur.get("batch") or cur.get("legacy_guard") or engines:
            keep.append((p, rec))
        elif now - float(seen.get(key, now)) > (MERGE_IDLE_TTL_S if _holder_tag(cur).startswith("merge") else IDLE_RECLAIM_S):
            log(f"[host-lease] reclaimed idle holder {_holder_tag(cur)} after {now - float(seen[key]):.0f} s: {_describe(cur)}")
            _remove_records([(p, cur)])
        else:
            marks[key] = seen.get(key, now)
            keep.append((p, rec))
    if marks != seen:
        _write_record(side, marks)
    return keep


def acquire(label: str, *, wait: bool = True, timeout_s: Optional[float] = None, kill_orphans: bool = False,
            log: Callable[[str], None] = _log_default,
            sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic,
            scan: Optional[Callable[..., List[Tuple[int, str]]]] = None, exclusive: Optional[bool] = None) -> None:
    """Hold the host lease, or raise HostLeaseBusy. See the module docstring for the rules."""
    global _registered
    if disabled(log):
        log(f"[host-lease] OFF ({OFF_ENV}=off, owner): {label} launches without the host lease")
        return
    check_gate_quiet(log)
    exclusive = _exclusive(exclusive) or any(word in (label + " " + sys.argv[0]).lower()
                                            for word in ("perf", "fps", "frame-time", "frametime", "tracy"))
    if holds():
        if exclusive and not all(_ours(_read(slot_path(i))) for i in range(slot_count())):
            raise HostLeaseBusy("cannot upgrade a held normal slot; release it before requesting exclusive")
        return
    folders = _folders_held_here()
    if folders:
        raise LockOrderError(f"lock order: {label} (pid {os.getpid()}) asks for a host game slot while it holds the deploy-folder "
                             f"lease of {', '.join(folders)}; take the slot first (smoke_lib/host_lease.py LOCK ORDER), "
                             "or pass --headless for a run that needs no slot")
    scan = scan or foreign_engines
    if timeout_s is None:
        timeout_s = float(os.environ.get(TIMEOUT_ENV) or DEFAULT_TIMEOUT_S)
    path = lease_path().with_suffix(".queue.json")
    deadline = clock() + timeout_s
    last_report = -REPORT_EVERY_S
    # Merge-gate stages and exclusive holds queue ahead of pass/fail runs; an exclusive at the head admits nobody behind it.
    priority = 0 if exclusive or os.environ.get("MC2_IN_GATE") == "1" else 1
    ticket = {"pid": os.getpid(), "created": child_registry.process_create_time(os.getpid()), "exclusive": exclusive,
              "priority": priority, "label": label, "argv": sys.argv[:3], "cwd": os.getcwd()}
    granted = False
    try:
        while True:
            with _queue_lock():
                queue = [t for t in (_read(path) or {}).get("tickets", []) if _holder_alive(t)]
                if ticket not in queue:
                    queue.append(ticket)
                queue.sort(key=lambda t: t.get("priority", 1))   # stable: FIFO within a class
                records = _records()
                dead = [(p, rec) for p, rec in records
                        if rec and p.exists() and not _holder_alive(rec)
                        and not (_read(p) or {}).get("legacy_guard")]
                for _p, rec in dead:
                    log(f"[host-lease] reclaiming slot of dead holder: {_describe(rec)}")
                _remove_records(dead)
                live = [(p, r) for p, r in records if _holder_alive(r)]
                foreign = scan() if queue[0] == ticket else []
                if queue[0] == ticket:
                    live = _reclaim_idle(live, foreign, log)
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
                free = [i for i in eligible_slots(device_hint())
                        if (not slot_path(i).exists() or (_read(slot_path(i)) or {}).get("legacy_guard"))
                        and not any(affinity_mask(i) & mask for mask in occupied)]
                legacy = any("slot" not in rec for _, rec in live) or (lease_path().exists() and _read(lease_path()) is None)
                eligible = queue[0] == ticket and not foreign and not legacy and not (priority == 1 and gate_window_blocks()) and (not live if exclusive else
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
                        _write_record(slot_path(i), dict(rec, slot=i, exclusive=exclusive, device=slot_device(i),
                                                       cores_per_slot=cores_per_slot(),
                                                       affinity_mask=sum(affinity_mask(j) for j in range(slot_count())) if exclusive else affinity_mask(i)))
                        allocated.append((slot_path(i), _read(slot_path(i))))
                    if eligible:
                        try:
                            _refresh_game_affinity()
                        except BaseException:
                            _remove_records(allocated)
                            raise
                        queue.pop(0)
                _write_record(path, {"tickets": queue})
                if eligible:
                    granted = True
                    if not _registered:
                        atexit.register(release)
                        _registered = True
                    return
                why = ("a game this process did not start is running: " + ", ".join(f"{img} pid {pid}" for pid, img in foreign)
                       if foreign else "the host game lease is held by " + _describe(live[0][1] if live else (queue[0] if queue and queue[0] != ticket else None)))
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
        _refresh_game_affinity()
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

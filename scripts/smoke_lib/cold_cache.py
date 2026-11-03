# scripts/smoke_lib/cold_cache.py
"""Make an install's files cold in the Windows file cache before a launch (--cold-cache).

Players start the game with the install's files NOT in RAM; a smoke run that follows a previous run reads them from the standby list and
so under-reports load time. This module evicts them, strongest method first, and PROVES the result by timing a read of the install's
largest file before and after (a flush that did not take shows up as a ratio near 1 and is reported, never claimed).

Methods, in order:
  1. purge_standby_list  -- NtSetSystemInformation(SystemMemoryListInformation, MemoryPurgeStandbyList). Needs an elevated token
                            (SeProfileSingleProcessPrivilege). System-wide: it also cools other processes' cached files, which is
                            harmless for a smoke host.
  2. evict_by_reading    -- no admin: read a large unrelated file set (default 24 GiB, MC2_COLD_EVICT_GB) so the cache manager reuses
                            the standby pages. Slow (minutes) and only as good as the probe says; used only when method 1 is unavailable.
If neither makes the probe cold, ColdResult.ok is False and .note says so (the run proceeds and the summary carries the warning).
"""
from __future__ import annotations

import ctypes
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Optional

PROBE_MIN_BYTES = 8 * 1024 * 1024
PROBE_MAX_BYTES = 256 * 1024 * 1024   # the probe is read 3 times: keep it quick (the install's .pdb can be gigabytes)
COLD_RATIO = 1.5            # hot-read speed / post-flush read speed must be at least this for the flush to count
SYSTEM_MEMORY_LIST_INFORMATION = 80
MEMORY_PURGE_STANDBY_LIST = 4
SE_PROFILE_SINGLE_PROCESS = "SeProfileSingleProcessPrivilege"


@dataclass
class ColdResult:
    method: str = "none"
    ok: bool = False
    note: str = ""
    probe_file: str = ""
    probe_warm_mb_s: float = 0.0     # read speed right before the flush (page cache hot, after a priming read)
    probe_after_mb_s: float = 0.0    # read speed right after the flush
    seconds: float = 0.0
    extra: dict = field(default_factory=dict)

    def as_dict(self) -> dict:
        return {"method": self.method, "ok": self.ok, "note": self.note, "probe_file": self.probe_file,
                "probe_warm_mb_s": round(self.probe_warm_mb_s, 1), "probe_after_mb_s": round(self.probe_after_mb_s, 1),
                "seconds": round(self.seconds, 2), **self.extra}


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def install_files(install_dir: Path) -> list[Path]:
    out: list[Path] = []
    for root, _dirs, files in os.walk(install_dir):
        for f in files:
            out.append(Path(root) / f)
    return out


def _read_all(path: Path, chunk: int = 4 * 1024 * 1024) -> tuple[int, float]:
    t0 = time.perf_counter()
    n = 0
    with open(path, "rb", buffering=0) as fh:
        while True:
            b = fh.read(chunk)
            if not b:
                break
            n += len(b)
    return n, time.perf_counter() - t0


def _mb_s(nbytes: int, secs: float) -> float:
    return (nbytes / (1024 * 1024)) / secs if secs > 0 else float("inf")


def pick_probe(files: Iterable[Path]) -> Optional[Path]:
    best: Optional[Path] = None
    best_sz = 0
    for p in files:
        try:
            sz = p.stat().st_size
        except OSError:
            continue
        if best_sz < sz <= PROBE_MAX_BYTES:
            best, best_sz = p, sz
    return best if best is not None and best_sz >= PROBE_MIN_BYTES else None


def purge_standby_list() -> tuple[bool, str]:
    """True when the kernel accepted the standby-list purge."""
    if sys.platform != "win32":
        return False, "not windows"
    if not is_admin():
        return False, "not elevated (SeProfileSingleProcessPrivilege needs admin)"
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    ntdll = ctypes.WinDLL("ntdll")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)

    class LUID(ctypes.Structure):
        _fields_ = [("Low", ctypes.c_uint32), ("High", ctypes.c_int32)]

    class TP(ctypes.Structure):
        _fields_ = [("Count", ctypes.c_uint32), ("Luid", LUID), ("Attr", ctypes.c_uint32)]

    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    token = ctypes.c_void_p()
    if not advapi.OpenProcessToken(ctypes.c_void_p(kernel.GetCurrentProcess()), 0x28, ctypes.byref(token)):   # ADJUST_PRIVILEGES | QUERY
        return False, "OpenProcessToken failed"
    luid = LUID()
    if not advapi.LookupPrivilegeValueW(None, SE_PROFILE_SINGLE_PROCESS, ctypes.byref(luid)):
        return False, "LookupPrivilegeValue failed"
    tp = TP(1, luid, 2)   # SE_PRIVILEGE_ENABLED
    advapi.AdjustTokenPrivileges(token, False, ctypes.byref(tp), 0, None, None)
    if ctypes.get_last_error() != 0:   # ERROR_NOT_ALL_ASSIGNED (1300): the token does not hold the privilege
        return False, f"privilege not held (winerr {ctypes.get_last_error()})"
    cmd = ctypes.c_int(MEMORY_PURGE_STANDBY_LIST)
    st = ntdll.NtSetSystemInformation(SYSTEM_MEMORY_LIST_INFORMATION, ctypes.byref(cmd), ctypes.sizeof(cmd))
    if st != 0:
        return False, f"NtSetSystemInformation status 0x{st & 0xFFFFFFFF:08x}"
    return True, "standby list purged"


def evict_by_reading(skip_under: Path, gigabytes: float, roots: Iterable[Path]) -> tuple[int, str]:
    """Read unrelated files until `gigabytes` have been read; returns (bytes_read, note). Never touches files under skip_under."""
    target = int(gigabytes * (1 << 30))
    total = 0
    skip = str(skip_under.resolve()).lower()
    for root in roots:
        for base, _dirs, files in os.walk(root):
            if str(Path(base).resolve()).lower().startswith(skip):
                continue
            for f in files:
                p = Path(base) / f
                try:
                    if p.stat().st_size < PROBE_MIN_BYTES:
                        continue
                    n, _ = _read_all(p)
                except OSError:
                    continue
                total += n
                if total >= target:
                    return total, f"read {total >> 20} MiB of unrelated files"
    return total, f"only {total >> 20} MiB of unrelated files were available"


def make_cold(install_dir: Path, evict_roots: Iterable[Path] = (), evict_gb: Optional[float] = None) -> ColdResult:
    t0 = time.perf_counter()
    res = ColdResult()
    probe = pick_probe(install_files(install_dir))
    if probe is None:
        res.note = "no probe file >= 8 MiB under the install; nothing to flush or verify"
        return res
    res.probe_file = str(probe)
    n, _ = _read_all(probe)                      # prime
    n, s = _read_all(probe)                      # hot read
    res.probe_warm_mb_s = _mb_s(n, s)

    ok, note = purge_standby_list()
    res.method = "purge_standby_list" if ok else "none"
    res.note = note
    if not ok:
        gb = evict_gb if evict_gb is not None else float(os.environ.get("MC2_COLD_EVICT_GB", "24"))
        roots = list(evict_roots)
        if gb > 0 and roots:
            got, enote = evict_by_reading(install_dir, gb, roots)
            res.method = "evict_by_reading"
            res.note = f"{note}; {enote}"
            res.extra["evict_bytes"] = got

    n, s = _read_all(probe)                      # the probe read itself warms the file again: it is the LAST thing done here
    res.probe_after_mb_s = _mb_s(n, s)
    res.ok = res.probe_after_mb_s > 0 and res.probe_warm_mb_s / res.probe_after_mb_s >= COLD_RATIO
    if not res.ok and res.method != "none":
        res.note += f"; probe did not slow down ({res.probe_warm_mb_s:.0f} -> {res.probe_after_mb_s:.0f} MB/s): the cache was not flushed"
    if res.method == "purge_standby_list":
        purge_standby_list()                     # the verification read re-cached the probe file: flush again so the launch starts cold
    res.seconds = time.perf_counter() - t0
    return res

# scripts/smoke_lib/deploy_lease.py
"""Deploy-folder checkout/lease system for run_smoke.py.

Prevents concurrent smoke sessions from fighting over the same deploy folder.
A lease is an entry in a shared JSON registry at LEASE_REGISTRY_PATH.

LEASE REGISTRY FORMAT (A:/Games/mc2-opengl/.smoke_leases.json):
  {
    "folder_key": {
      "folder": "/abs/path/to/deploy/folder",
      "pid": 12345,
      "acquired_utc": "2026-06-23T12:00:00Z",
      "last_used_utc": "2026-06-23T12:00:01Z"
    },
    ...
  }

The "folder_key" is the casefold'd, forward-slash normalized absolute folder path.

STALE RULE: a lease whose last_used_utc is older than LEASE_TTL_SECS (default 3600)
is considered free and may be taken over.

ATOMIC WRITES: write to a .tmp sibling, then os.replace().
CONTENTION: retry up to _WRITE_RETRIES times with short sleep on OSError.
"""
from __future__ import annotations

import contextlib
import json
import os
import socket
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional
from . import nofocus  # noqa: E402  (OWNER RULE: no window for any child process)

# The lane names and install paths live in ONE model (tools/repo_intel/lane_model.py); this module only decides
# which of them the unattended walk may lease.
import sys as _sys  # noqa: E402
_sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "repo_intel"))
import lane_model  # noqa: E402

# ── Constants ──────────────────────────────────────────────────────────────────

# Shared registry path — MUST be an absolute path outside any worktree so every
# concurrent smoke session (in any worktree) sees the same file.
LEASE_REGISTRY_PATH = Path("A:/Games/mc2-opengl/.smoke_leases.json")

# Default lease TTL in seconds (1 hour). Override via MC2_SMOKE_LEASE_TTL_SECS.
_DEFAULT_TTL_SECS = 3600

# Retry parameters for atomic writes under contention.
_WRITE_RETRIES = 8
_WRITE_RETRY_SLEEP = 0.05  # seconds

# Canonical deploy folders in preference order.
# (name -> absolute path string)
# 2026-10-05 reset: one dev lane walks automatically; another builder lane (lane2) is leased only by name
# (--deploy lane2), never picked by the walk. Older lanes are archived to E:/mc2-archive-2026-10/installs.
# The golden install (0.4c) is NOT walked: it is a visual-golden reference whose exe predates the
# background-launch hardening, and an LRU walk that fell onto it put a fullscreen window on the owner's desktop
# (2026-10-10). A busy dev lane is an error, never a reason to pick a different install.
_WALK_LANES = ("0.4",)
DEPLOY_FOLDERS: list[tuple[str, str]] = list(lane_model.install_paths())

_NAME_TO_PATH: dict[str, str] = dict(DEPLOY_FOLDERS)


# ── Helpers ────────────────────────────────────────────────────────────────────

def _ttl() -> int:
    """Return the effective TTL in seconds (env override or default)."""
    try:
        return int(os.environ.get("MC2_SMOKE_LEASE_TTL_SECS", ""))
    except (ValueError, TypeError):
        return _DEFAULT_TTL_SECS


def _folder_key(folder: str) -> str:
    """Stable dict key: casefold + forward-slash normalized absolute path."""
    try:
        return str(Path(folder).resolve()).replace("\\", "/").casefold()
    except Exception:
        return folder.replace("\\", "/").casefold()


def _now_utc() -> str:
    return datetime.now(tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _parse_utc(s: str) -> Optional[float]:
    """Parse an ISO-8601 UTC timestamp string to a POSIX timestamp float.
    Returns None on failure."""
    if not s:
        return None
    for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%dT%H:%M:%S+00:00"):
        try:
            return datetime.strptime(s, fmt).replace(tzinfo=timezone.utc).timestamp()
        except ValueError:
            continue
    return None


def _age_secs(last_used_utc: str) -> Optional[float]:
    """Return seconds since last_used_utc, or None if unparseable."""
    ts = _parse_utc(last_used_utc)
    if ts is None:
        return None
    return time.time() - ts


def _is_stale(entry: dict) -> bool:
    """True if the lease's last_used_utc is older than TTL (expired/stale)."""
    age = _age_secs(entry.get("last_used_utc", ""))
    if age is None:
        return True  # unparseable = treat as stale
    return age > _ttl()


def _folder_has_exe(folder: str) -> bool:
    """True if the folder exists on disk and contains mc2.exe."""
    p = Path(folder)
    return p.is_dir() and (p / "mc2.exe").is_file()


# ── Registry I/O ───────────────────────────────────────────────────────────────

def _read_registry() -> dict:
    """Load the lease registry JSON.  Returns {} if missing or corrupt."""
    try:
        text = LEASE_REGISTRY_PATH.read_text(encoding="utf-8")
        data = json.loads(text)
        if not isinstance(data, dict):
            return {}
        return data
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {}


def _write_registry(data: dict) -> None:
    """Atomically write the registry to disk (write-tmp + os.replace).

    Retries up to _WRITE_RETRIES times on OSError (e.g., sharing violation
    from a concurrent writer on Windows).
    """
    LEASE_REGISTRY_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = LEASE_REGISTRY_PATH.with_suffix(".json.tmp")
    text = json.dumps(data, indent=2, ensure_ascii=False)
    for attempt in range(_WRITE_RETRIES):
        try:
            tmp.write_text(text, encoding="utf-8")
            os.replace(str(tmp), str(LEASE_REGISTRY_PATH))
            return
        except OSError:
            if attempt < _WRITE_RETRIES - 1:
                time.sleep(_WRITE_RETRY_SLEEP)
            else:
                raise


# ── Cross-process lock ───────────────────────────────────────────────────────
# A sidecar lock file guards the whole read-modify-write of the registry so two
# concurrent sessions cannot both select the same free folder (the check-then-
# write race). O_CREAT|O_EXCL is atomic on Windows and POSIX. A lock held longer
# than _LOCK_STALE_SECS is presumed crashed and stolen, so a dead session never
# deadlocks the pool. _lock_path() is computed at call time so tests that
# reassign LEASE_REGISTRY_PATH get a matching lock path.

_LOCK_STALE_SECS = 30
_LOCK_WAIT_SECS = 10


def _lock_path() -> Path:
    return LEASE_REGISTRY_PATH.with_suffix(".lock")


@contextlib.contextmanager
def _registry_lock():
    lp = _lock_path()
    lp.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + _LOCK_WAIT_SECS
    acquired = False
    while True:
        try:
            fd = os.open(str(lp), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            try:
                os.write(fd, str(os.getpid()).encode())
            finally:
                os.close(fd)
            acquired = True
            break
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(lp) > _LOCK_STALE_SECS:
                    os.unlink(str(lp))  # steal a crashed holder's lock
                    continue
            except OSError:
                pass
            if time.time() >= deadline:
                # Give up waiting rather than deadlock; proceed best-effort.
                _log("[LEASE] WARN: registry lock wait timed out — proceeding unlocked")
                break
            time.sleep(0.05)
    try:
        yield
    finally:
        if acquired:
            try:
                os.unlink(str(lp))
            except OSError:
                pass


# ── Owner identity ─────────────────────────────────────────────────────────────

_GIT_INFO_CACHE: Optional[dict] = None


def _worktree_root() -> str:
    # deploy_lease.py lives at <worktree>/scripts/smoke_lib/deploy_lease.py
    try:
        return str(Path(__file__).resolve().parents[2])
    except Exception:
        return os.getcwd()


def _git_info() -> dict:
    """Best-effort (branch, sha) for the worktree this script runs from. Cached."""
    global _GIT_INFO_CACHE
    if _GIT_INFO_CACHE is not None:
        return _GIT_INFO_CACHE
    info = {"branch": None, "sha": None}
    root = _worktree_root()
    for field, args in (("branch", ["rev-parse", "--abbrev-ref", "HEAD"]),
                        ("sha", ["rev-parse", "--short", "HEAD"])):
        try:
            out = subprocess.run(["git", "-C", root, *args],
                                capture_output=True, text=True, timeout=5,
                                **nofocus.hidden_kwargs())
            info[field] = out.stdout.strip() or None
        except Exception:
            pass
    _GIT_INFO_CACHE = info
    return info


def _owner_record(folder: str, now: str, intended_test: Optional[str]) -> dict:
    gi = _git_info()
    return {
        "folder": str(Path(folder).resolve()).replace("\\", "/"),
        "pid": os.getpid(),
        "hostname": socket.gethostname(),
        "worktree": _worktree_root(),
        "branch": gi.get("branch"),
        "sha": gi.get("sha"),
        "intended_test": intended_test,
        "acquired_utc": now,
        "last_used_utc": now,
    }


# ── Public API ─────────────────────────────────────────────────────────────────

class LeaseError(RuntimeError):
    """Raised when a requested folder is busy and not stale."""


class NoFolderAvailable(RuntimeError):
    """Raised when all preferred+fallback folders are busy and fresh."""


def _acquire_locked(folder: str, intended_test: Optional[str]) -> str:
    """Read-check-write the registry for `folder`. Caller MUST hold _registry_lock().

    Takes over stale leases. Raises LeaseError if held by a live lease.
    """
    key = _folder_key(folder)
    reg = _read_registry()
    entry = reg.get(key)
    if entry and not _is_stale(entry):
        age = _age_secs(entry.get("last_used_utc", ""))
        age_min = f"{int(age // 60)}m" if age is not None else "?m"
        raise LeaseError(
            f"folder '{folder}' is busy (owner pid {entry.get('pid')}, "
            f"host {entry.get('hostname')}, {age_min} old)"
        )
    if entry:
        age = _age_secs(entry.get("last_used_utc", ""))
        age_min = f"{int(age // 60)}m" if age is not None else "?m"
        _log(f"[LEASE] {_short_name(folder)} stale ({age_min}) -> taking over")
    reg[key] = _owner_record(folder, _now_utc(), intended_test)
    _write_registry(reg)
    return folder


def acquire_lease(folder: str, intended_test: Optional[str] = None) -> str:
    """Acquire the lease for `folder`, taking over stale leases (atomic).

    Returns the acquired folder path. Raises LeaseError if held by a live lease.
    Low-level primitive; callers normally use auto_acquire().
    """
    with _registry_lock():
        return _acquire_locked(folder, intended_test)


def release_lease(folder: str) -> None:
    """Release the lease for `folder`.  No-op if not held."""
    key = _folder_key(folder)
    with _registry_lock():
        reg = _read_registry()
        if key in reg:
            del reg[key]
            _write_registry(reg)


def touch_lease(folder: str) -> None:
    """Refresh the last_used_utc timestamp for an active lease (heartbeat)."""
    key = _folder_key(folder)
    with _registry_lock():
        reg = _read_registry()
        if key in reg:
            reg[key]["last_used_utc"] = _now_utc()
            _write_registry(reg)


def _acquire_waiting(folder: str, intended_test: Optional[str], what: str, wait_s: float) -> None:
    """Take the lease for `folder`; while a live lease holds it, wait up to `wait_s` seconds (visible countdown) before raising the busy LeaseError."""
    from . import bounded_wait
    last: list = []

    def ready() -> str:
        try:
            with _registry_lock():
                _acquire_locked(folder, intended_test)
            return ""
        except LeaseError as exc:
            last[:] = [exc]
            return str(exc)

    if not bounded_wait.wait_until(ready, what, wait_s):
        raise last[0]


def auto_acquire(
    explicit_folder: Optional[str] = None,
    deploy_name: Optional[str] = None,
    intended_test: Optional[str] = None,
    wait_s: float = 0.0,
) -> str:
    """Auto-select and acquire a deploy folder (atomic — whole selection is
    performed under a single registry lock, so no two sessions can pick the
    same free folder).

    Priority:
      1. deploy_name: use that named folder (error if busy+fresh, after waiting up to `wait_s` seconds for the holder).
      2. explicit_folder: lease the folder that --exe points into (same wait).
      3. Otherwise: walk DEPLOY_FOLDERS in preference order, skip missing,
         skip busy+fresh, take first free/stale.
    """
    if deploy_name is not None:
        if deploy_name not in _NAME_TO_PATH:
            raise LeaseError(
                f"unknown deploy name '{deploy_name}'; "
                f"valid names: {', '.join(_NAME_TO_PATH)}"
            )
        folder = _NAME_TO_PATH[deploy_name]
        if not _folder_has_exe(folder):
            raise LeaseError(
                f"--deploy {deploy_name}: folder not found or missing mc2.exe: {folder}"
            )
        _acquire_waiting(folder, intended_test, f"deploy folder {deploy_name}", wait_s)
        _log(f"[LEASE] acquired {deploy_name} (--deploy)")
        return folder

    if explicit_folder is not None:
        folder = str(Path(explicit_folder).resolve().parent)
        _acquire_waiting(folder, intended_test, f"deploy folder {_short_name(folder)}", wait_s)
        _log(f"[LEASE] acquired {_short_name(folder)} (--exe)")
        return folder

    # Auto-select — LRU ORDER (least-recently-used folder picked first):
    # never-leased folders rank as oldest (picked before any leased one); among
    # leased candidates, the one with the oldest last_used_utc wins. This spreads
    # load across deploy folders instead of biasing toward a fixed preference
    # order. Selection+write happens under ONE lock so it's atomic.
    with _registry_lock():
        reg = _read_registry()
        busy_entries: list[str] = []

        # Build candidate list: (name, folder, last_used_ts_or_None) for folders
        # that actually exist and have an mc2.exe.
        candidates: list[tuple[str, str, Optional[float]]] = []
        for name, folder in DEPLOY_FOLDERS:
            if name not in _WALK_LANES:
                continue
            if not _folder_has_exe(folder):
                _log(f"[LEASE] {name} ({folder}) skipped — folder missing or no mc2.exe")
                continue
            entry = reg.get(_folder_key(folder))
            ts = _parse_utc(entry.get("last_used_utc", "")) if entry else None
            candidates.append((name, folder, ts))

        # LRU sort: never-leased (ts=None) ranks oldest -> picked first; then
        # ascending timestamp (older leased folders before newer ones). Stable
        # tiebreak by DEPLOY_FOLDERS declaration order via the candidates list
        # already being built in that order.
        candidates.sort(key=lambda c: (c[2] is not None, c[2] if c[2] is not None else 0.0))

        # Walk LRU-ordered, take first non-busy. If a folder is busy and fresh,
        # log and try next; if free or stale, acquire.
        for name, folder, _ts in candidates:
            entry = reg.get(_folder_key(folder))
            if entry and not _is_stale(entry):
                age = _age_secs(entry.get("last_used_utc", ""))
                age_min = f"{int(age // 60)}m" if age is not None else "?m"
                _log(f"[LEASE] {name} busy (owner pid {entry.get('pid')}, "
                     f"host {entry.get('hostname')}, {age_min} old) -> trying next (LRU)")
                busy_entries.append(f"{name} (pid={entry.get('pid')}, age={age_min})")
                continue
            _acquire_locked(folder, intended_test)
            label = "never used" if _ts is None else ("stale" if entry else "free")
            _log(f"[LEASE] acquired {name} via LRU (was {label})")
            return folder

    holders = "; ".join(busy_entries) if busy_entries else "all folders missing mc2.exe"
    raise NoFolderAvailable(
        f"[LEASE] ERROR: all deploy folders busy or unavailable. Holders: {holders}. "
        f"Another smoke is running: wait for it. A lease left by a killed run is "
        f"cleared with `py -3 -m scripts.smoke_lib.deploy_lease --clear-dead` "
        f"(removes only leases whose owner pid is dead); never bypass with --no-lease."
    )


def _pid_alive(pid: int) -> bool:
    """True if a process with this pid exists on this host (never signals it)."""
    from . import child_registry
    return child_registry.pid_alive(pid)


def clear_dead_leases(pid_alive=_pid_alive, hostname: Optional[str] = None) -> list:
    """Remove leases whose owning process is gone. Returns the removed folder keys.

    This is the sanctioned recovery for a lease left behind by a killed run:
    only an entry written by THIS host whose pid is no longer alive is removed.
    A live owner's lease, and any lease from another host, is left alone -- a
    busy lease means another smoke is running and the right move is to wait.
    """
    host = hostname if hostname is not None else socket.gethostname()
    removed = []
    with _registry_lock():
        reg = _read_registry()
        for key, entry in list(reg.items()):
            try:
                pid = int(entry.get("pid") or 0)
            except (TypeError, ValueError):
                pid = 0
            if entry.get("hostname") != host:
                continue
            if pid_alive(pid):
                continue
            del reg[key]
            removed.append(key)
        if removed:
            _write_registry(reg)
    return removed


def _short_name(folder: str) -> str:
    """Return the short name for a folder path, or a truncated path."""
    norm = str(Path(folder).resolve()).replace("\\", "/").casefold()
    for name, path in DEPLOY_FOLDERS:
        if _folder_key(path) == norm:
            return name
    # Fallback: last two path components
    parts = Path(folder).parts
    return "/".join(parts[-2:]) if len(parts) >= 2 else folder


def _log(msg: str) -> None:
    """Print a lease log line to stderr (same channel as runner messages)."""
    import sys
    print(f"[runner] {msg}", file=sys.stderr)


if __name__ == "__main__":
    import sys
    if "--clear-dead" in sys.argv[1:]:
        gone = clear_dead_leases()
        print(f"[lease] cleared {len(gone)} dead-owner lease(s)" + (": " + ", ".join(gone) if gone else ""))
        sys.exit(0)
    print("usage: py -3 -m scripts.smoke_lib.deploy_lease --clear-dead", file=sys.stderr)
    sys.exit(2)

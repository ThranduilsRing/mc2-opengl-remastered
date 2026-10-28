#!/usr/bin/env python3
"""savegame_backup - keep the owner's quicksave safe while a fixture run overwrites it.

WHAT: `backup` copies ~/.mechcommander2/savegame/testgame.ims to a sibling `testgame.ims.bak_<YYYYMMDD_HHMMSS>` and writes a pending marker beside it;
`restore` puts the backed-up file back (or removes the fixture's file when there was none) and clears the marker. A second `backup` while a marker is
pending keeps the FIRST backup: the file then in place is a fixture's, not the owner's.
WHY: the quicksave/quickload fixtures (MC2_BOOT_QUICKSAVE_AT / MC2_BOOT_QUICKLOAD_AT) use the same file as the pause-menu quicksave, so every fixture
run replaces whatever the owner saved. Backups are kept after a restore, but only the newest BACKUP_KEEP (5) are kept: older
`testgame.ims.bak_<YYYYMMDD_HHMMSS>` files are deleted right after a new one is written; a hand-named copy such as
`testgame.ims.bak_before_boss` never matches and is never deleted. The save itself and every other file are never touched.

  py -3 tools/savegame_backup.py backup  [--dir D]
  py -3 tools/savegame_backup.py restore [--dir D]
  py -3 tools/savegame_backup.py status  [--dir D]
  py -3 tools/savegame_backup.py selftest

Used as a library by tools/saveload_roundtrip.py (`guarded()` context manager).
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import json
import os
import re
import shutil
import sys
import tempfile
import uuid


SAVE_NAME = "testgame.ims"
MARKER_NAME = SAVE_NAME + ".backup-pending"
BACKUP_KEEP = 5
BACKUP_RE = re.compile(r"^" + re.escape(SAVE_NAME) + r"\.bak_\d{8}_\d{6}$")


def default_dir() -> str:
    return os.path.join(os.path.expanduser("~"), ".mechcommander2", "savegame")


@contextlib.contextmanager
def scratch_dir():
    """A throwaway directory for the selftest and the unit tests. -> its path; removed on exit.

    os.mkdir instead of mkdtemp: mkdtemp creates the directory 0700, which some restricted Windows hosts (sandboxed
    agents, hardened ACLs) refuse to write into. The random name keeps concurrent runs apart.
    """
    d = os.path.join(tempfile.gettempdir(), "mc2-savegame-backup-" + uuid.uuid4().hex)
    os.mkdir(d)
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)


def prune_backups(directory: str, keep: int = BACKUP_KEEP, protect: str | None = None) -> list[str]:
    """Delete all but the newest `keep` timestamped backups. -> the deleted names.

    Only `testgame.ims.bak_YYYYMMDD_HHMMSS` regular files are candidates: the save, the marker, the lock and
    hand-named copies (e.g. `testgame.ims.bak_before_boss`) are never touched. `protect` names the backup just
    written; it survives even when its stamp sorts oldest, so a clock that went backwards can leave `keep` + 1
    files until the next backup.
    """
    found = []
    with contextlib.suppress(OSError):
        for name in os.listdir(directory):
            if BACKUP_RE.match(name) and os.path.isfile(os.path.join(directory, name)):
                found.append(name)
    found.sort(reverse=True)
    keep = max(keep, 0)
    kept = found[:keep]
    if protect in found and protect not in kept:
        kept.append(protect)
    removed = []
    for name in [n for n in found if n not in kept]:
        with contextlib.suppress(OSError):
            os.remove(os.path.join(directory, name))
            removed.append(name)
    return removed


def backup(directory: str, now: datetime.datetime | None = None) -> str:
    """Back the quicksave up. -> a one-line description. Idempotent while a marker is pending."""
    marker = os.path.join(directory, MARKER_NAME)
    if os.path.exists(marker):
        with open(marker, encoding="utf-8") as fh:
            prior = json.load(fh)
        return f"backup already pending ({prior.get('backup') or 'no save existed'}); kept the first one"
    os.makedirs(directory, exist_ok=True)
    src = os.path.join(directory, SAVE_NAME)
    name = None
    if os.path.exists(src):
        stamp = (now or datetime.datetime.now()).strftime("%Y%m%d_%H%M%S")
        name = f"{SAVE_NAME}.bak_{stamp}"
        shutil.copy2(src, os.path.join(directory, name))
        prune_backups(directory, protect=name)
    with open(marker, "w", encoding="utf-8") as fh:
        json.dump({"backup": name}, fh)
    return f"backed up to {name}" if name else "no quicksave existed; restore will remove the fixture's file"


def restore(directory: str) -> str:
    """Put the backed-up quicksave back. -> a one-line description. A restore with nothing pending is a no-op."""
    marker = os.path.join(directory, MARKER_NAME)
    if not os.path.exists(marker):
        return "nothing pending; left the quicksave alone"
    with open(marker, encoding="utf-8") as fh:
        name = json.load(fh).get("backup")
    dst = os.path.join(directory, SAVE_NAME)
    if name:
        src = os.path.join(directory, name)
        if not os.path.exists(src):
            return f"backup {name} is missing; marker kept, quicksave not touched"
        shutil.copy2(src, dst)
        msg = f"restored {name}"
    else:
        with contextlib.suppress(FileNotFoundError):
            os.remove(dst)
        msg = "removed the fixture's quicksave (none existed before)"
    os.remove(marker)
    return msg


def status(directory: str) -> str:
    marker = os.path.join(directory, MARKER_NAME)
    if not os.path.exists(marker):
        return "no backup pending"
    with open(marker, encoding="utf-8") as fh:
        return "backup pending: " + str(json.load(fh).get("backup"))


LOCK_NAME = SAVE_NAME + ".run-lock"
#: A run that died holding the lock must not block the host forever: a lock older than this is stolen. Waits are bounded too.
#: The PID check below catches a dead holder immediately; this bound covers a lock whose holder record is unreadable.
LOCK_STALE_S = 2 * 60 * 60
LOCK_WAIT_S = 25 * 60


def _pid_alive(pid: int) -> bool:
    """True when a process with this pid exists on this host. Never sends a signal."""
    if pid <= 0:
        return False
    try:
        import psutil
        return psutil.pid_exists(pid) and psutil.Process(pid).status() != psutil.STATUS_ZOMBIE
    except ImportError:
        pass
    except Exception:   # psutil.Error includes AccessDenied, which still means the pid exists
        return True
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        handle = k32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        try:
            code = ctypes.c_ulong(0)
            return bool(k32.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 259
        finally:
            k32.CloseHandle(handle)
    try:
        os.kill(int(pid), 0)    # POSIX only: signal 0 sends nothing
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _lock_holder_pid(path: str) -> int | None:
    try:
        with open(path, encoding="utf-8") as fh:
            for field in fh.read().split():
                if field.startswith("pid="):
                    return int(field[4:])
    except (OSError, ValueError):
        pass
    return None


@contextlib.contextmanager
def run_lock(directory: str, wait_s: float = LOCK_WAIT_S, stale_s: float = LOCK_STALE_S):
    """One save-fixture run at a time per save directory (all saveload runs of every lane and of the merge gate share ~/.mechcommander2/savegame:
    W77's saveload-mc2_10 quickload crashed on a save another run had just overwritten). Exclusive-create lock file, bounded wait, stale steal."""
    import time
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, LOCK_NAME)
    deadline = time.monotonic() + wait_s
    while True:
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"pid={os.getpid()} since={time.time():.0f}".encode())
            os.close(fd)
            break
        except FileExistsError:
            try:
                age = time.time() - os.path.getmtime(path)
            except OSError:
                continue
            holder = _lock_holder_pid(path)
            if holder is not None and not _pid_alive(holder):
                with contextlib.suppress(OSError):
                    os.remove(path)
                print(f"savegame_backup: took over stale lock {path}: pid {holder} is gone", flush=True)
                continue
            if age > stale_s:
                with contextlib.suppress(OSError):
                    os.remove(path)
                print(f"savegame_backup: took over stale lock {path}: held {age / 60:.0f} min (limit {stale_s / 60:.0f})", flush=True)
                continue
            if time.monotonic() > deadline:
                raise TimeoutError(f"savegame_backup: save directory {directory} stayed locked by another save run for {wait_s:.0f} s")
            time.sleep(2)
    try:
        yield
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


@contextlib.contextmanager
def guarded(directory: str | None = None):
    """Take the save-directory run lock, back up on entry, restore on exit (also on an exception)."""
    d = directory or default_dir()
    with run_lock(d):
        print("savegame_backup: " + backup(d), flush=True)
        try:
            yield d
        finally:
            print("savegame_backup: " + restore(d), flush=True)


def selftest() -> int:
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    def read(d, n=SAVE_NAME):
        with open(os.path.join(d, n), "rb") as fh:
            return fh.read()

    t0 = datetime.datetime(2026, 10, 11, 18, 0, 0)
    with scratch_dir() as d:
        with open(os.path.join(d, SAVE_NAME), "wb") as fh:
            fh.write(b"owner")
        backup(d, t0)
        expect(read(d, SAVE_NAME + ".bak_20261011_180000") == b"owner", "backup copies the owner's save under a timestamped sibling name")
        with open(os.path.join(d, SAVE_NAME), "wb") as fh:
            fh.write(b"fixture one")
        expect("kept the first" in backup(d, t0 + datetime.timedelta(seconds=5)), "a second backup while pending keeps the first")
        expect(not os.path.exists(os.path.join(d, SAVE_NAME + ".bak_20261011_180005")), "a second backup writes no new file")
        restore(d)
        expect(read(d) == b"owner", "restore puts the owner's save back")
        expect(not os.path.exists(os.path.join(d, MARKER_NAME)), "restore clears the marker")
        expect(os.path.exists(os.path.join(d, SAVE_NAME + ".bak_20261011_180000")), "the backup file is kept after a restore")
        expect("nothing pending" in restore(d), "a restore with nothing pending changes nothing")
        expect(read(d) == b"owner", "a no-op restore leaves the save alone")
    with scratch_dir() as d:
        backup(d, t0)
        with open(os.path.join(d, SAVE_NAME), "wb") as fh:
            fh.write(b"fixture")
        restore(d)
        expect(not os.path.exists(os.path.join(d, SAVE_NAME)), "with no owner save, restore removes the fixture's file")
    with scratch_dir() as d:
        with open(os.path.join(d, SAVE_NAME), "wb") as fh:
            fh.write(b"owner")
        try:
            with guarded(d):
                with open(os.path.join(d, SAVE_NAME), "wb") as fh:
                    fh.write(b"fixture")
                raise RuntimeError("run failed")
        except RuntimeError:
            pass
        expect(read(d) == b"owner", "guarded() restores when the run raises")
        with run_lock(d):
            try:
                with run_lock(d, wait_s=0.1):
                    expect(False, "a second run must not enter while the lock is held")
            except TimeoutError:
                pass
        expect(not os.path.exists(os.path.join(d, LOCK_NAME)), "the lock is released on exit")
        with open(os.path.join(d, LOCK_NAME), "w") as fh:
            fh.write("dead")
        os.utime(os.path.join(d, LOCK_NAME), (0, 0))
        with run_lock(d, wait_s=0.1):
            pass   # a stale lock is stolen
    for f in fails:
        print("savegame_backup selftest FAIL: " + f)
    print("savegame_backup selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["backup", "restore", "status", "selftest"])
    ap.add_argument("--dir", default=None, help="savegame directory (default ~/.mechcommander2/savegame)")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    d = args.dir or default_dir()
    print({"backup": backup, "restore": restore, "status": status}[args.cmd](d))
    return 0


if __name__ == "__main__":
    sys.exit(main())

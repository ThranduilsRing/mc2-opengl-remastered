#!/usr/bin/env python3
"""savegame_backup - keep the owner's quicksave safe while a fixture run overwrites it.

WHAT: `backup` copies ~/.mechcommander2/savegame/testgame.ims to a sibling `testgame.ims.bak_<YYYYMMDD_HHMMSS>` and writes a pending marker beside it;
`restore` puts the backed-up file back (or removes the fixture's file when there was none) and clears the marker. A second `backup` while a marker is
pending keeps the FIRST backup: the file then in place is a fixture's, not the owner's.
WHY: the quicksave/quickload fixtures (MC2_BOOT_QUICKSAVE_AT / MC2_BOOT_QUICKLOAD_AT) use the same file as the pause-menu quicksave, so every fixture
run replaces whatever the owner saved. Backups are kept after a restore: they are the owner's data, and only the owner prunes them.

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
import shutil
import sys
import tempfile

SAVE_NAME = "testgame.ims"
MARKER_NAME = SAVE_NAME + ".backup-pending"


def default_dir() -> str:
    return os.path.join(os.path.expanduser("~"), ".mechcommander2", "savegame")


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


@contextlib.contextmanager
def guarded(directory: str | None = None):
    """Back up on entry, restore on exit (also on an exception)."""
    d = directory or default_dir()
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
    with tempfile.TemporaryDirectory() as d:
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
    with tempfile.TemporaryDirectory() as d:
        backup(d, t0)
        with open(os.path.join(d, SAVE_NAME), "wb") as fh:
            fh.write(b"fixture")
        restore(d)
        expect(not os.path.exists(os.path.join(d, SAVE_NAME)), "with no owner save, restore removes the fixture's file")
    with tempfile.TemporaryDirectory() as d:
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

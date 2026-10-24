"""Read-only warning for Windows Firewall notifications belonging to this exe."""
from __future__ import annotations

import json
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    from ..console_child import child_kwargs
except ImportError:    # top-level smoke_lib with scripts/ on sys.path
    from console_child import child_kwargs


def matching_pids(rows, exe: Path) -> set[int]:
    wanted = str(exe.resolve()).replace("/", "\\").casefold()
    matches = set()
    for row in rows:
        cmd = row.get("CommandLine") or ""
        if not re.search(r"FirewallControlPanel\.dll\s*,\s*ShowNotificationDialog\b", cmd, re.I):
            continue
        args = [a or b for a, b in re.findall(r'"([^"]*)"|(\S+)', cmd)]
        if any(a.replace("/", "\\").casefold() == wanted for a in args):
            matches.add(int(row["ProcessId"]))
    return matches


def query(exe: Path) -> set[int]:
    command = (
        "Get-CimInstance Win32_Process -Filter \"Name = 'rundll32.exe'\" "
        "-ErrorAction Stop | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        capture_output=True, text=True, timeout=5, check=True,
        **child_kwargs(redirected=True))
    rows = json.loads(result.stdout or "[]")
    return matching_pids([rows] if isinstance(rows, dict) else (rows or []), exe)


class Guard:
    def __init__(self, exe: Path):
        self.exe = exe
        self.seen: set[int] = set()
        self.next_poll = 0.0
        self.failed = False

    def poll(self, *, final=False) -> None:
        if sys.platform != "win32" or self.failed:
            return
        now = time.monotonic()
        if not final and now < self.next_poll:
            return
        self.next_poll = now + 2
        try:
            pids = query(self.exe)
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            self.failed = True
            print(f"[firewall] WARN dialog query failed: {exc}", file=sys.stderr)
            return
        for pid in sorted(pids - self.seen):
            print(f"[firewall] WARN FirewallControlPanel dialog pid={pid} exe={self.exe}",
                  file=sys.stderr)
        self.seen.update(pids)

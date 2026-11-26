# scripts/smoke_lib/crash_delta.py
"""A crash bundle is a red for every harness, whatever its row said (gate-devlost-1).

WHAT: `log_crash(text)` names the crash a capture log announces; `Watch(install).new_real()` lists the real bundles that appeared under
<install>/crashes/ while a gate row or a smoke mission ran; `real(dir)` is the one classification both share.
WHY: both stage-1126f gates lost the Vulkan device in capture-after (2026-11-26): the pose read MISSING / "no sidecar", the row was ADVISORY,
the verdict PASS and the build went to v0.4 (A:/Games/mc2-ops/studies/gate-health-2026-11-26.md P0-1/P0-2).
"""
from __future__ import annotations

import re
import threading
from pathlib import Path
from typing import Optional, Sequence, Tuple

from .logparse import CRASH_PATTERNS, SENTINEL_RECORD

_FATAL = re.compile(r"^fatal backend exit \d+: (.+)$")
_TAGGED = re.compile(r"^\[(?:CRASHBUNDLE|CRASH)\] (?:FAST-EXIT )?(.*?)\s*(?:bundle=\S.*)?$")
_BUNDLE = re.compile(r"^\[(?:CRASHBUNDLE|CRASH)\] .*?\bbundle=(\S.*?)\s*$")
_MINIDUMP = re.compile(r"^(?:Minidump: |\[CRASH\] minidump=)(\S.*?)\s*$")
_LOCK = threading.Lock()
_REPORTED = set()   # bundle paths already handed out: two gate rows that overlapped one crash blame it once


def log_crash(text: str) -> "Optional[Tuple[str, str]]":
    """(reason, bundle dir) of the crash a log announces, or None when it announces none.

    The reason is the engine's `fatal backend exit N: <why>` line, else the text of its `[CRASHBUNDLE]` / `[CRASH] FAST-EXIT` tag, else the
    first line matching logparse.CRASH_PATTERNS. The bundle dir comes from a `bundle=` or minidump line ('' when the log names none). The
    `unclean exit` record of an earlier harness-killed session is printed by the next launch: it is not that process's crash."""
    lines = [ln.strip() for ln in (text or "").splitlines() if SENTINEL_RECORD not in ln]
    hit = next((ln for ln in lines if any(p.search(ln) for p in CRASH_PATTERNS)), None)
    if hit is None:
        return None

    def first(pattern: "re.Pattern[str]") -> "Optional[str]":
        match = next((m for ln in lines if (m := pattern.search(ln))), None)
        return match.group(1) if match else None

    bundle = first(_BUNDLE)
    if bundle is None and (dump := first(_MINIDUMP)):
        bundle = str(Path(dump).parent)
    return (first(_FATAL) or first(_TAGGED) or hit)[:200], bundle or ""


def real(bundle: Path) -> "Optional[str]":
    """The Reason line of a real bundle dir: one holding a minidump.dmp, or a crash.txt whose `Reason:` line is not the sentinel record that
    logparse.SENTINEL_RECORD names (a harness-killed game, W135 3274: informational). None for any other dir (nothing crashed there)."""
    try:
        lines = (Path(bundle) / "crash.txt").read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        lines = []
    reason = next((ln[len("Reason:"):].strip() for ln in lines if ln.startswith("Reason:")), "")
    if not (Path(bundle) / "minidump.dmp").is_file() and (not reason or reason.startswith(SENTINEL_RECORD)):
        return None
    code = next((" ".join(ln.split()[:2]) for ln in lines if ln.startswith("Code: ")), "")
    return reason or code or "minidump.dmp"


def row_install(command: "Sequence[str]") -> "Optional[Path]":
    """The install a gate row runs the game from: the dir holding the file after --exe, else the dir after --install. None: the row names none."""
    for flag, parent in (("--exe", True), ("--install", False)):
        at = list(command).index(flag) + 1 if flag in command else 0
        if 0 < at < len(command):
            path = Path(command[at])
            return path.parent if parent else path
    return None


def _names(crashes: "Optional[Path]") -> set:
    try:
        return {p.name for p in crashes.iterdir() if p.is_dir()}
    except (AttributeError, OSError):
        return set()


class Watch:
    """The crash dirs of an install as they are now; new_real() lists (path, reason) of the real bundles that appeared since, each once."""

    def __init__(self, install: "Optional[Path]"):
        self.crashes = Path(install) / "crashes" if install else None
        self.before = _names(self.crashes)

    def new_real(self) -> "list[tuple[str, str]]":
        found = []
        with _LOCK:
            for path in sorted(self.crashes / n for n in _names(self.crashes) - self.before):
                reason = real(path)
                if reason and str(path) not in _REPORTED:
                    _REPORTED.add(str(path))
                    found.append((str(path), reason))
        return found

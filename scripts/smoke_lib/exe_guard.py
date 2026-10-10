# scripts/smoke_lib/exe_guard.py
"""Refuse to launch an mc2.exe that cannot keep itself off the user's desktop.

WHY THIS EXISTS (owner incident 2026-10-10)
-------------------------------------------
A smoke tier ran against the golden install, whose exe predates the
background-launch hardening (CURSOR-GRAB-FIX-1, NOFOCUS-HARDEN-1). It came up
fullscreen on the owner's desktop and fought him for the mouse. The harness
cannot hide a window the exe itself sizes to the monitor: only an exe that
understands MC2_WINDOW_OFFSCREEN can be launched without taking the screen.

So the runner and the capture tool check the exe BEFORE launching:

  * the exe binary must contain the MC2_WINDOW_OFFSCREEN gate name (the
    capability itself, which a manifest cannot misreport), and
  * a game exe must carry a build fingerprint (scripts/exe_fingerprint.py) whose
    commit is a descendant of MIN_COMMIT (the commit that introduced the
    cursor-grab fix). The deploy manifest's src_commit is NOT trusted: it is git
    HEAD at deploy time, so a stale exe copied out of a current tree would pass.
    No fingerprint means refuse. The editor exe has no fingerprint and is judged
    by the capability string alone.

nofocus.spawn()/run() call this for mc2.exe and the editor exes, so every
harness launcher is covered, not only the ones that remembered to ask.

The golden install is additionally never a smoke target: it is a reference for
visual goldens, not a build under test.

MC2_OWNER_VISIBLE_OK=1 overrides the staleness refusal. It is the OWNER's
switch for a deliberate visible run; agents never set it.
"""
from __future__ import annotations

import csv
import json
import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Mapping, Optional

try:    # package context (scripts.smoke_lib)
    from ..console_child import child_kwargs
except ImportError:    # top-level smoke_lib with scripts/ on sys.path
    from console_child import child_kwargs

#: First commit whose exe keeps the cursor and window off the user's desktop.
MIN_COMMIT = "ffebf5e87"

#: Gate name an exe must carry to be launchable offscreen.
OFFSCREEN_MARK = b"MC2_WINDOW_OFFSCREEN"

#: Installs that are visual-golden references, never smoke targets.
GOLDEN_INSTALL_NAMES = frozenset({"mc2-win64-v0.4c"})

OWNER_OVERRIDE_ENV = "MC2_OWNER_VISIBLE_OK"

_CSV_MANIFEST = ".deployed_manifest.csv"
_JSON_MANIFEST = ".deploy-manifest.json"

_REPO_ROOT = Path(__file__).resolve().parents[2]

GitAncestor = Callable[[str, str], Optional[bool]]


def read_src_commit(install_dir) -> Optional[str]:
    """src_commit recorded by the install's newest deploy manifest, or None.

    A trailing `-dirty` marker (dirty-tree deploy) is kept: it is part of the
    identity being logged, and is stripped only for the ancestry test.
    """
    d = Path(install_dir)
    cands = [p for p in (d / _CSV_MANIFEST, d / _JSON_MANIFEST) if p.is_file()]
    if not cands:
        return None
    newest = max(cands, key=lambda p: p.stat().st_mtime)
    try:
        if newest.suffix == ".csv":
            with open(newest, newline="", encoding="utf-8") as fh:
                for row in csv.reader(fh):
                    if not row or row[0] in ("manifest_version", "relpath"):
                        continue
                    if len(row) >= 4 and row[3]:
                        return row[3]
            return None
        with open(newest, encoding="utf-8") as fh:
            doc = json.load(fh)
        return doc.get("src_commit") or doc.get("head") or doc.get("git_head") or None
    except (OSError, ValueError, csv.Error):
        return None


def exe_has_offscreen(exe, chunk: int = 8 << 20) -> bool:
    """True when the exe binary contains the MC2_WINDOW_OFFSCREEN gate name."""
    overlap = len(OFFSCREEN_MARK) - 1
    tail = b""
    try:
        with open(exe, "rb") as fh:
            while True:
                block = fh.read(chunk)
                if not block:
                    return False
                window = tail + block
                if OFFSCREEN_MARK in window:
                    return True
                tail = window[-overlap:]
    except OSError:
        return False


def git_is_ancestor(base: str, sha: str) -> Optional[bool]:
    """`base` is an ancestor of `sha` in this repo; None when git cannot say."""
    try:
        r = subprocess.run(["git", "-C", str(_REPO_ROOT), "merge-base", "--is-ancestor", base, sha],
                           capture_output=True, timeout=20, **child_kwargs(redirected=True))
    except Exception:
        return None
    if r.returncode == 0:
        return True
    if r.returncode == 1:
        return False
    return False    # unknown object: the commit is not in this history


def is_golden(exe) -> bool:
    return Path(exe).resolve().parent.name.casefold() in GOLDEN_INSTALL_NAMES


GAME_EXE_NAMES = frozenset({"mc2.exe", "mc2-asan.exe"})
EDITOR_EXE_NAMES = frozenset({"editrel.exe", "mission editor.exe"})


def engine_kind(exe) -> Optional[str]:
    """"game" / "editor" for the engine executables the harness must vet, else None."""
    name = Path(str(exe)).name.casefold()
    if name in GAME_EXE_NAMES:
        return "game"
    if name in EDITOR_EXE_NAMES:
        return "editor"
    return None


_FP_CACHE: dict = {}


def embedded_commit(exe) -> "tuple[Optional[str], str]":
    """-> (commit compiled into the exe, reason it is unavailable).

    The commit is read from the binary itself (scripts/exe_fingerprint.py). The deploy
    manifest's src_commit is NOT used: deploy_payload records git HEAD at deploy time, so a
    stale exe copied out of a current tree would look current.
    """
    p = Path(exe)
    st = p.stat()
    key = (str(p), st.st_mtime_ns, st.st_size)
    if key not in _FP_CACHE:
        try:
            from .. import exe_fingerprint
        except ImportError:
            import exe_fingerprint
        _FP_CACHE[key] = exe_fingerprint.extract(p)
        fp = _FP_CACHE[key]
        if fp.get("present"):
            import sys as _sys
            how = ("MC2FP1 literal" if fp.get("method") == "mc2fp1-literal/v1"
                   else "operand-walk fallback (build predates the MC2FP1 literal)")
            print(f"[exe-guard] {p.parent.name}: fingerprint read via {how}", file=_sys.stderr)
    fp = _FP_CACHE[key]
    if not fp.get("present"):
        return None, f"{fp.get('reason', 'unreadable')}: {fp.get('detail', '')}".strip()
    sha = fp.get("sha")
    if not sha or sha == "unknown":
        return None, "the build fingerprint carries no commit (built outside git)"
    return sha + ("-dirty" if fp.get("dirty") else ""), ""


@dataclass
class Verdict:
    exe: str
    install: str
    src_commit: Optional[str]
    reasons: List[str] = field(default_factory=list)
    manifest_commit: Optional[str] = None

    @property
    def ok(self) -> bool:
        return not self.reasons

    def banner(self) -> str:
        return f"[SMOKE] exe={self.exe} src_commit={self.src_commit or 'unknown'}"

    def refusal(self) -> str:
        return (f"[exe-guard] REFUSING to launch install '{self.install}' "
                f"(embedded commit {self.src_commit or 'unknown'}): " + " ".join(self.reasons))


def check_exe(exe, *, purpose: str = "smoke", env: Optional[Mapping[str, str]] = None,
              is_ancestor: GitAncestor = git_is_ancestor,
              has_offscreen: Callable[[object], bool] = exe_has_offscreen,
              commit_of: Callable[[object], "tuple[Optional[str], str]"] = embedded_commit) -> Verdict:
    """Judge whether `exe` may be launched. `purpose` is "smoke" (golden refused), "capture" or "spawn".

    A game exe must carry a build fingerprint whose commit descends from MIN_COMMIT; an exe
    with no fingerprint is refused (nothing proves what it is). The editor exe has no
    fingerprint, so it is judged by the MC2_WINDOW_OFFSCREEN capability alone.
    """
    env = os.environ if env is None else env
    exe_p = Path(exe).resolve()
    kind = engine_kind(exe_p) or "game"
    v = Verdict(exe=str(exe_p), install=exe_p.parent.name, src_commit=None,
                manifest_commit=read_src_commit(exe_p.parent))
    why_unknown = ""
    if kind == "game":
        v.src_commit, why_unknown = commit_of(exe_p)
    else:
        v.src_commit = "n/a (editor)"

    if purpose == "smoke" and is_golden(exe_p):
        v.reasons.append("the golden install is a visual-golden reference and is never a smoke target; "
                         "pass --deploy 0.4 (or the --exe of a current build).")

    if env.get(OWNER_OVERRIDE_ENV) != "1":
        stale = []
        if not has_offscreen(exe_p):
            stale.append(f"the exe lacks the {OFFSCREEN_MARK.decode()} capability, so it would open a "
                         f"visible window on the user's desktop.")
        if kind == "game":
            if v.src_commit is None:
                stale.append(f"the exe has no usable embedded build fingerprint ({why_unknown}); "
                             f"nothing proves it postdates {MIN_COMMIT}.")
            elif is_ancestor(MIN_COMMIT, v.src_commit.removesuffix("-dirty")) is False:
                stale.append(f"embedded commit {v.src_commit} is not a descendant of {MIN_COMMIT} "
                             f"(the background-launch fix); redeploy a current build.")
        if stale:
            stale.append(f"Only the owner may override this, with {OWNER_OVERRIDE_ENV}=1.")
        v.reasons.extend(stale)
    return v

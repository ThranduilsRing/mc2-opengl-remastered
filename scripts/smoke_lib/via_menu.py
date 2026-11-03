# scripts/smoke_lib/via_menu.py
"""Run the game the way a player starts it: boot, main menu, campaign, mission (run_smoke --via-menu, tracy_capture_compare --via-menu).

Every other smoke/determinism/load-time/Tracy run boots straight into `--mission <stem>` under MC2_SMOKE_MODE, so it never pays the
front end (menu build, campaign start, logistics/mission-begin) a player does. This module drives the REAL front end with the same
mechanism scripts/sweep_campaign.py uses: no MC2_SMOKE_MODE, no --mission, MC2_BOOT_TO_BAY=<campaign fit> (the main menu starts the
campaign by itself; campaign mission 1 goes straight to the mission). No synthetic input is sent, so nothing can touch the owner's focus.

What is measured, all from engine stdout (MC2_LOG=1) stamped with the harness wall clock:
  time_to_menu_s     spawn -> first `[VIA_MENU v1] event=main_menu`
  mission_load_ms    last `[MISSION] t=` of the load (mission_load_start is t=0), engine clock
  first60            `[FIRST60 v1] n= avg_ms= p50_ms= p99_ms= max_ms= first_ms=` (the first 60 frames after mission_ready)
  time_to_first60_s  spawn -> the `[FIRST60 v1]` line

Not driven: the main menu's intro movie. MC2_BOOT_TO_BAY starts the campaign on the menu's first update, before the intro movie exists, so
front-end videos are skipped (engine event `[VIDEO] event=skip reason=boot_to_bay`). `skip_intro` additionally sets MC2_MENU_CANARY_SKIP_INTRO.
"""
from __future__ import annotations

import os
import re
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

from . import nofocus

STOCK_CAMPAIGN = "campaign"
MENU_RE = re.compile(r"\[VIA_MENU v1\] event=main_menu")
FIRST60_RE = re.compile(r"\[FIRST60 v1\] n=(?P<n>\d+) avg_ms=(?P<avg>[\d.]+) p50_ms=(?P<p50>[\d.]+) p99_ms=(?P<p99>[\d.]+) "
                        r"max_ms=(?P<max>[\d.]+) first_ms=(?P<first>[\d.]+)")
MISSION_RE = re.compile(r"\[MISSION\] t=\s*(?P<t>[\d.]+)s\s+phase=(?P<name>\S+)")
STRIP_ENV = ("MC2_SMOKE_MODE", "MC2_HEARTBEAT", "MC2_SMOKE_SEED", "MC2_SMOKE_FIXED_TIMESTEP", "MC2_BOOT_TO_MISSION", "MC2_BOOT_TO_SCREEN",
             "MC2_SOAK_AUTOWIN")


def apply_via_menu_env(env: dict, campaign: str = STOCK_CAMPAIGN, skip_intro: bool = False) -> dict:
    """Mutate env into the front-end boot (also used by tracy_capture_compare). Returns env."""
    for k in STRIP_ENV:
        env.pop(k, None)
    env["MC2_BOOT_TO_BAY"] = campaign
    env["MC2_LOG"] = "1"                 # without it stdout goes to NUL and no marker survives
    env["MC2_NO_CURSOR_GRAB"] = "1"
    if skip_intro:
        env["MC2_MENU_CANARY_SKIP_INTRO"] = "1"
    return env


def game_argv(exe: Path) -> list[str]:
    """No --mission, no --duration: the front end picks the mission, the harness decides when the run is over."""
    return [str(exe), "--profile", "stock"]


@dataclass
class ViaMenuStats:
    time_to_menu_s: Optional[float] = None
    mission_load_ms: Optional[float] = None
    time_to_first60_s: Optional[float] = None
    first60: dict = field(default_factory=dict)
    video_skips: int = 0
    crashed: bool = False

    def complete(self) -> bool:
        return self.time_to_menu_s is not None and bool(self.first60)

    def as_dict(self) -> dict:
        return {"time_to_menu_s": self.time_to_menu_s, "mission_load_ms": self.mission_load_ms,
                "time_to_first60_s": self.time_to_first60_s, "first60": self.first60, "video_skips": self.video_skips,
                "crashed": self.crashed, "complete": self.complete()}


def parse_stamped(lines: "list[tuple[float, str]]") -> ViaMenuStats:
    """lines: (seconds since spawn, text). Pure so the tests need no engine."""
    st = ViaMenuStats()
    for t, text in lines:
        if st.time_to_menu_s is None and MENU_RE.search(text):
            st.time_to_menu_s = round(t, 3)
        m = MISSION_RE.search(text)
        if m:
            st.mission_load_ms = round(float(m.group("t")) * 1000.0, 1)
        m = FIRST60_RE.search(text)
        if m and not st.first60:
            st.first60 = {"n": int(m.group("n")), **{k: float(m.group(k)) for k in ("avg", "p50", "p99", "max", "first")}}
            st.time_to_first60_s = round(t, 3)
        if "event=skip" in text and "[VIDEO]" in text:
            st.video_skips += 1
        if text.startswith("CRASH:") or "MC2 Remastered Crash Report" in text:
            st.crashed = True
    return st


def run_via_menu(exe: Path, out_dir: Path, timeout_s: float = 150.0, campaign: str = STOCK_CAMPAIGN, skip_intro: bool = False,
                 extra_env: Optional[dict] = None, settle_s: float = 1.0, show_window: bool = True,
                 spawn: Callable = nofocus.spawn) -> dict:
    """Launch, wait for [FIRST60 v1] (or exit / timeout), end the game, return the summary dict. Log in out_dir/via_menu.log."""
    out_dir.mkdir(parents=True, exist_ok=True)
    env = apply_via_menu_env(os.environ.copy(), campaign, skip_intro)
    env.update(extra_env or {})
    t0 = time.perf_counter()
    proc = spawn(game_argv(exe), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, errors="replace",
                 cwd=str(exe.resolve().parent), env=env, show_window=show_window)
    lines: list[tuple[float, str]] = []
    done = threading.Event()

    def pump():
        for raw in proc.stdout:
            text = raw.rstrip("\r\n")
            lines.append((time.perf_counter() - t0, text))
            if FIRST60_RE.search(text):
                done.set()
        done.set()   # EOF: the process ended

    th = threading.Thread(target=pump, daemon=True)
    th.start()
    done.wait(timeout=timeout_s)
    time.sleep(settle_s)
    timed_out = not any(FIRST60_RE.search(t) for _, t in lines) and proc.poll() is None
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    th.join(timeout=5)
    (out_dir / "via_menu.log").write_text("\n".join(f"{t:9.3f} {x}" for t, x in lines), encoding="utf-8", errors="replace")
    st = parse_stamped(lines)
    result = "pass" if st.complete() and not st.crashed else "fail"
    reason = "" if result == "pass" else ("timeout" if timed_out else "crash" if st.crashed else "no_first60" if st.time_to_menu_s else "no_menu")
    return {"mode": "via_menu", "result": result, "reason": reason, "campaign": campaign, "skip_intro": skip_intro,
            "wall_s": round(time.perf_counter() - t0, 2), **st.as_dict()}

#!/usr/bin/env python3
"""SCREEN-SETTLE-SHOT-1: screenshot a front-end / logistics screen after it has rendered N settled frames, then exit.

    py -3 scripts/screen_shot.py --screen logistics --exe <install>/mc2.exe --out-dir <dir>
    py -3 scripts/run_visual_capture.py --screen mechbay@45 --exe ... --out-dir ...     (same thing)
    py -3 tools/box_run.py --gpu 0 -- screen:campaign_missions                            (box row)

Screen ids: campaign_missions, logistics (briefing, with the tactical minimap), purchase, mechbay (bay), loadout, pilots (launch), load.
`<id>@N` = settle frames (default 30). Engine side: MC2_SHOT_SCREEN (docs/tier1_env_vars.md, code/screen_settle_capture.h).
Exit 0 = PNG/TGA written and not black; 1 = engine reported FAIL (screen never reached, ImGui not ready, write failed);
2 = no verdict line; 3 = frame is black; 4 = exe missing.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mc2_env_pin as env_pin  # noqa: E402

KNOWN_SCREENS = ("campaign_missions", "logistics", "briefing", "purchase", "mechbay", "bay", "loadout", "pilots", "launch", "load", "encyclopedia", "mainmenu", "loadgame")
_LINE = re.compile(r"\[SCREEN_SHOT v1\] (OK|FAIL)\b(.*)")


def parse_screen(text: str) -> tuple[str, int]:
    """'<id>[@N]' -> (id, N). ValueError on an unknown id or bad N (mirrors the engine's parseSpec)."""
    name, _, n = text.partition("@")
    if name.lower() not in KNOWN_SCREENS:
        raise ValueError(f"unknown screen id {name!r}; known: {', '.join(KNOWN_SCREENS)}")
    if "@" in text and not (n.isdigit() and 1 <= int(n) <= 999999):
        raise ValueError(f"bad settle frame count in {text!r}")
    return name.lower(), int(n) if n else 30


def build_env(screen: str, out_tga: Path, *, campaign: str = "campaign", mission: str = "mc2_03",
              width: int = 1920, height: int = 1080, declared: dict | None = None) -> tuple[dict, dict]:
    """Pinned launch environment (no ambient MC2_*), MC2_BOOT_TO_BAY puts the campaign at the logistics grid."""
    name, settle = parse_screen(screen)
    sets = {
        "MC2_BOOT_TO_BAY": campaign,
        "MC2_BOOT_TO_MISSION": mission,   # campaign mission 1 skips logistics; a later mission lands on the grid
        "MC2_MENU_CANARY_SKIP_INTRO": "1",
        "MC2_SHOT_SCREEN": f"{name}@{settle}",
        "MC2_SCREENSHOT_PATH": str(out_tga),
        "MC2_RENDER_BACKEND": "vulkan",
        "MC2_LOG": "1",
        "MC2_NO_CURSOR_GRAB": "1",
        "MC2_WIDTH": str(width),
        "MC2_HEIGHT": str(height),
    }
    if name == "loadout":
        sets.update(MC2_BOOT_TO_SCREEN="bay", MC2_BOOT_TO_MECHLAB="1", MC2_SOAK_AUTO_PURCHASE="1",
                    MC2_SOAK_AUTOWIN="1", MC2_SOAK_STOP_AT_BAY="1")
    elif name in ("encyclopedia", "mainmenu", "loadgame"):
        for key in ("MC2_BOOT_TO_BAY", "MC2_BOOT_TO_MISSION", "MC2_SOAK_AUTOWIN", "MC2_SOAK_STOP_AT_BAY"):
            sets.pop(key, None)
        sets["MC2_BOOT_TO_SCREEN"] = name
    return env_pin.build_pinned_env(sets, declared)


def parse_verdict(text: str) -> tuple[str | None, str]:
    """Last '[SCREEN_SHOT v1] OK|FAIL ...' line -> (verdict, rest) or (None, '')."""
    found: tuple[str | None, str] = (None, "")
    for line in text.splitlines():
        m = _LINE.search(line)
        if m:
            found = (m.group(1), m.group(2).strip())
    return found


def tga_is_black(path: Path) -> bool:
    """True when an uncompressed or RLE-less 24/32-bit TGA has no non-black pixel (sampled). Unreadable counts as black."""
    try:
        b = path.read_bytes()
        idlen, cmap, itype = b[0], b[1], b[2]
        bpp = b[16]
        if cmap or itype not in (2, 10) or bpp not in (24, 32):
            return b[18 + idlen:].count(0) != len(b) - 18 - idlen  # unknown layout: raw byte test
        data = b[18 + idlen:]
        return not any(data[i] for i in range(0, len(data), 7))
    except (OSError, IndexError):
        return True


def run(exe: Path, screen: str, out_dir: Path, *, timeout: float = 240.0, campaign: str = "campaign",
        width: int = 1920, height: int = 1080, declared: dict | None = None, show_window: bool = True) -> int:
    from smoke_lib import nofocus  # noqa: E402  (imports win32 helpers lazily; keep the module importable for tests)
    if not exe.exists():
        print(f"[screen_shot] ERROR exe missing: {exe}", file=sys.stderr)
        return 4
    name, _ = parse_screen(screen)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_tga = out_dir / f"screen_{name}.tga"
    log = out_dir / f"screen_{name}.log"
    out_tga.unlink(missing_ok=True)
    env, _rec = build_env(screen, out_tga, campaign=campaign, width=width, height=height, declared=declared)
    with open(log, "w", encoding="utf-8", errors="replace") as fp:
        proc = nofocus.spawn([str(exe)], stdout=fp, stderr=subprocess.STDOUT, cwd=str(exe.parent), env=env,
                             show_window=show_window, small_window=False)
        t0 = time.time()
        while proc.poll() is None and time.time() - t0 < timeout:
            time.sleep(0.5)
        if proc.poll() is None:
            proc.kill()
    verdict, rest = parse_verdict(log.read_text(encoding="utf-8", errors="replace"))
    print(f"[screen_shot] screen={screen} verdict={verdict} {rest}")
    if verdict is None:
        print(f"[screen_shot] FAIL: no [SCREEN_SHOT v1] line (engine without MC2_SHOT_SCREEN, crash or timeout); log {log}")
        return 2
    if verdict == "FAIL":
        return 1
    if not out_tga.is_file() or tga_is_black(out_tga):
        print(f"[screen_shot] FAIL: {out_tga} missing or black")
        return 3
    print(f"[screen_shot] OK {out_tga}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--screen", required=True, help="<id>[@settle frames]")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--campaign", default="campaign", help="campaign fit basename (MC2_BOOT_TO_BAY)")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--env-declare", action="append", default=[], metavar="MC2_KEY=VALUE")
    a = ap.parse_args(argv)
    try:
        parse_screen(a.screen)
        declared = env_pin.parse_declarations(a.env_declare)
    except ValueError as e:
        print(f"[screen_shot] {e}", file=sys.stderr)
        return 2
    return run(Path(a.exe), a.screen, Path(a.out_dir), timeout=a.timeout, campaign=a.campaign,
               width=a.width, height=a.height, declared=declared)


if __name__ == "__main__":
    sys.exit(main())

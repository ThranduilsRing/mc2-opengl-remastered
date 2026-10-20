#!/usr/bin/env python3
"""sound_smoke - run one mission with a real SDL audio device and assert the engine opened audio and loaded its sounds.

WHAT: launches scripts/run_smoke.py for one mission with SDL_AUDIODRIVER set to a real device (never `dummy`, which
  smoke_lib/runner.py sets as a soft default), then parses the kept mission log for the audio-open line and for sound-load
  errors. It proves initialization and resource loading only -- audibility still needs one owner listen.
WHY: the normal smoke path is silent by design, so nothing at launch level ever proved the device opened or that SFX/music
  resources loaded.
USAGE (battery command line):
  py -3 tools/sound_smoke.py --deploy 0.4 --mission mc2_01 --duration 30
  py -3 tools/sound_smoke.py --log tests/smoke/artifacts/<stamp>/mc2_01.log   # re-grade an existing log
Exit 0 = PASS, 1 = FAIL (printed reason), 2 = configuration/launch problem.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent / "scripts"))
import console_child  # noqa: E402  (windowless console children)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RUN_SMOKE = ROOT / "scripts" / "run_smoke.py"

DEFAULT_STAGE = Path("G:/mc2-battery/sound")
INSTALL_ROOT = Path("A:/Games/mc2-opengl")
FORBIDDEN_INSTALLS = ("mc2-win64-v0.4c",)
FORBIDDEN_DEPLOYS = ("0.4c",)
DURATION_CAP = 30  # CLAUDE.md: the 30 s window is the soak/parity tier and is never exceeded.

# GameOS/gameos/gameos_sound.cpp:152 -- "Opened audio at <rate> Hz <bits> bit <layout> (<endian>), <n> bytes audio buffer"
OPEN_RE = re.compile(r"Opened audio at\s+(?P<rate>\d+)\s*Hz\s+(?P<bits>\d+)\s*bit\s+(?P<layout>\S+)",
                     re.IGNORECASE)
ARTIFACTS_RE = re.compile(r"\[runner\] artifacts:\s*(?P<dir>.+)")
# Advisory markers: the engine logs audio through MC2_LOG(Audio, ...) and the mixer through Mix_AllocateChannels.
MARKER_RE = re.compile(r"\[SOUND\]|\[Audio\]|Mix_AllocateChannels|channels\s*[=:]\s*\d+", re.IGNORECASE)

# Sound-load failures, matched against the literal strings in gameos_sound.cpp.
ERROR_PATTERNS = [
    re.compile(r"gosAudio_CreateResource:\s*Failed to load", re.IGNORECASE),
    re.compile(r"Failed to convert audio", re.IGNORECASE),
    re.compile(r"Failed to initialize SoundEngine", re.IGNORECASE),
    re.compile(r"makeAudio:\s*Failed to create audio resource", re.IGNORECASE),
    re.compile(r"missing\s+(?:wav|sound|audio)", re.IGNORECASE),
    re.compile(r"(?:sound|audio|wav|music)\s+(?:file\s+)?(?:not found|missing)", re.IGNORECASE),
]


def _parse_open(text: str) -> dict | None:
    """The audio-open line, or None when the device never opened."""
    m = OPEN_RE.search(text)
    if not m:
        return None
    return {"rate": int(m.group("rate")), "bits": int(m.group("bits")),
            "layout": m.group("layout")}


def _grade(text: str, require_markers: bool) -> tuple[list[str], list[str]]:
    """Return (failures, notes) for one mission log."""
    fails: list[str] = []
    notes: list[str] = []

    opened = _parse_open(text)
    if opened is None:
        fails.append("no 'Opened audio at <rate> Hz <bits> bit <layout>' line "
                     "(the device never opened)")
    else:
        notes.append(f"opened audio at {opened['rate']} Hz {opened['bits']} bit "
                     f"{opened['layout']}")
        if not 8000 <= opened["rate"] <= 192000:
            fails.append(f"implausible sample rate {opened['rate']} Hz")
        if opened["bits"] not in (8, 16, 24, 32):
            fails.append(f"implausible sample width {opened['bits']} bit")

    seen: set[str] = set()
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped in seen:
            continue
        if any(p.search(stripped) for p in ERROR_PATTERNS):
            seen.add(stripped)
            fails.append(f"sound-load error: {stripped[:200]}")

    markers = [l for l in text.splitlines() if MARKER_RE.search(l)]
    if markers:
        notes.append(f"{len(markers)} audio marker line(s)")
    elif require_markers:
        fails.append("no [SOUND]/[Audio]/Mix_AllocateChannels marker lines in the log")
    else:
        notes.append("note: no audio marker lines (pass --require-sound-markers to make this fatal)")

    return fails, notes


def _run(a) -> tuple[int, str, Path | None]:
    """Run run_smoke.py with a real audio driver. Returns (rc, combined output, artifact dir)."""
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = a.audio_driver
    cmd = [sys.executable, str(RUN_SMOKE), "--mission", a.mission,
           "--duration", str(a.duration), "--keep-logs"]
    if a.exe:
        cmd += ["--exe", a.exe]
    else:
        cmd += ["--deploy", a.deploy]
    print(f"[sound_smoke] SDL_AUDIODRIVER={a.audio_driver}: {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=str(ROOT),
                          **console_child.child_kwargs(redirected=True))
    text = (proc.stdout or "") + (proc.stderr or "")
    m = ARTIFACTS_RE.search(text)
    return proc.returncode, text, (Path(m.group("dir").strip()) if m else None)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mission", default="mc2_01", help="mission stem (default mc2_01)")
    ap.add_argument("--duration", type=int, default=DURATION_CAP,
                    help=f"run length in seconds, max {DURATION_CAP} (default {DURATION_CAP})")
    ap.add_argument("--deploy", default="0.4",
                    help="deploy folder short name passed to run_smoke.py (default 0.4)")
    ap.add_argument("--exe", default=None,
                    help="explicit mc2.exe; overrides --deploy")
    ap.add_argument("--audio-driver", default="directsound",
                    help="SDL_AUDIODRIVER for the child (default directsound; 'dummy' is refused)")
    ap.add_argument("--out-dir", default=str(DEFAULT_STAGE),
                    help=f"staging dir for the run log (default {DEFAULT_STAGE})")
    ap.add_argument("--log", default=None,
                    help="grade an existing mission log instead of launching anything")
    ap.add_argument("--require-sound-markers", action="store_true",
                    help="treat a missing [SOUND]/[Audio]/Mix_AllocateChannels marker as a failure")
    a = ap.parse_args()

    if a.audio_driver.strip().lower() == "dummy":
        print("sound_smoke: SDL_AUDIODRIVER=dummy proves nothing about device output; "
              "pick a real driver (e.g. directsound, wasapi)", file=sys.stderr)
        return 2
    if a.duration > DURATION_CAP:
        print(f"sound_smoke: --duration {a.duration} exceeds the {DURATION_CAP}s cap",
              file=sys.stderr)
        return 2
    if not a.log:
        if a.deploy and a.deploy.lower() in FORBIDDEN_DEPLOYS:
            print(f"sound_smoke: refusing --deploy {a.deploy} (the golden install)",
                  file=sys.stderr)
            return 2
        if a.exe and Path(a.exe).resolve().parent.name.lower() in FORBIDDEN_INSTALLS:
            print(f"sound_smoke: refusing --exe under the golden install "
                  f"{Path(a.exe).resolve().parent.name}", file=sys.stderr)
            return 2

    stage = Path(a.out_dir).resolve()
    try:
        stage.relative_to(INSTALL_ROOT.resolve())
        print(f"sound_smoke: refusing to stage under the game install ({stage}); "
              f"use {DEFAULT_STAGE}", file=sys.stderr)
        return 2
    except ValueError:
        pass
    stage.mkdir(parents=True, exist_ok=True)

    if a.log:
        log_path = Path(a.log)
        if not log_path.is_file():
            print(f"sound_smoke: log not found: {log_path}", file=sys.stderr)
            return 2
        run_rc = 0
        text = log_path.read_text(encoding="utf-8", errors="replace")
    else:
        run_rc, text, artifact_dir = _run(a)
        (stage / "run_smoke.log").write_text(text, encoding="utf-8", errors="replace")
        if artifact_dir is None:
            print("sound_smoke: run_smoke.py printed no artifact dir; see "
                  f"{stage / 'run_smoke.log'}", file=sys.stderr)
            return 2
        log_path = artifact_dir / f"{a.mission}.log"
        if not log_path.is_file():
            print(f"sound_smoke: mission log not written: {log_path} "
                  f"(run_smoke exit {run_rc})", file=sys.stderr)
            return 2
        text = log_path.read_text(encoding="utf-8", errors="replace")

    fails, notes = _grade(text, a.require_sound_markers)
    print(f"[sound_smoke] log: {log_path}")
    for note in notes:
        print(f"  {note}")
    if run_rc != 0:
        fails.append(f"run_smoke.py exited {run_rc} (the mission itself did not pass)")
    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("PASS: audio device opened and sound resources loaded. "
          "Audibility still needs one owner listen.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

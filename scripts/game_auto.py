"""MC2 game automation helper.

Usage:
  python game_auto.py launch           # start mc2.exe in background
  python game_auto.py kill             # taskkill mc2.exe
  python game_auto.py running          # print YES/NO
  python game_auto.py key <name> [hold_ms]
  python game_auto.py chord <mods+key>   e.g. alt+f1, ralt+8
  python game_auto.py click <x> <y>      absolute screen coords
  python game_auto.py move <x> <y>
  python game_auto.py wait <seconds>
  python game_auto.py screenshot <path>
  python game_auto.py script <file>      run a sequence (one cmd per line)

Sequence file format (one directive per line):
  wait 5
  key escape
  chord ralt+8
  click 960 540
  screenshot /tmp/shot.png
"""
import sys
import time
import subprocess
import os
from pathlib import Path

import pyautogui

GAME_EXE = r"A:\Games\mc2-opengl\mc2-win64-v0.1.1\mc2.exe"
GAME_DIR = r"A:\Games\mc2-opengl\mc2-win64-v0.1.1"

pyautogui.FAILSAFE = False  # don't abort when mouse hits corner
pyautogui.PAUSE = 0.05


def running() -> bool:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq mc2.exe", "/NH"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return "mc2.exe" in out
    except Exception:
        return False


def launch():
    if running():
        print("already running")
        return
    subprocess.Popen([GAME_EXE], cwd=GAME_DIR, creationflags=0x00000008)  # DETACHED_PROCESS
    print("launched")


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"], stderr=subprocess.DEVNULL)
    print("killed")


# Map of chord modifier names -> pyautogui keys
MODS = {
    "ctrl": "ctrl",
    "alt": "alt",
    "ralt": "altright",
    "lalt": "altleft",
    "shift": "shift",
    "rshift": "shiftright",
    "lshift": "shiftleft",
    "win": "win",
}


def do_key(name: str, hold_ms: int = 50):
    pyautogui.keyDown(name)
    time.sleep(hold_ms / 1000.0)
    pyautogui.keyUp(name)


def do_chord(spec: str):
    parts = spec.lower().split("+")
    mods = [MODS.get(p, p) for p in parts[:-1]]
    key = parts[-1]
    for m in mods:
        pyautogui.keyDown(m)
    pyautogui.keyDown(key)
    time.sleep(0.05)
    pyautogui.keyUp(key)
    for m in reversed(mods):
        pyautogui.keyUp(m)


def do_click(x: int, y: int):
    pyautogui.click(x, y)


def do_move(x: int, y: int):
    pyautogui.moveTo(x, y, duration=0.1)


def do_screenshot(path: str):
    img = pyautogui.screenshot()
    img.save(path)
    print(f"saved {path}")


def run_script(filepath: str):
    for line in Path(filepath).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        cmd = parts[0]
        args = parts[1:]
        print(f"> {line}")
        dispatch(cmd, args)


def dispatch(cmd: str, args):
    if cmd == "launch":
        launch()
    elif cmd == "kill":
        kill()
    elif cmd == "running":
        print("YES" if running() else "NO")
    elif cmd == "key":
        hold = int(args[1]) if len(args) > 1 else 50
        do_key(args[0], hold)
    elif cmd == "chord":
        do_chord(args[0])
    elif cmd == "click":
        do_click(int(args[0]), int(args[1]))
    elif cmd == "move":
        do_move(int(args[0]), int(args[1]))
    elif cmd == "wait":
        time.sleep(float(args[0]))
    elif cmd == "screenshot":
        do_screenshot(args[0])
    elif cmd == "script":
        run_script(args[0])
    else:
        print(f"unknown cmd: {cmd}")
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    dispatch(sys.argv[1], sys.argv[2:])

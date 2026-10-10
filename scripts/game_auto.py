"""MC2 game automation helper.

Usage:
  python game_auto.py launch           # start mc2.exe in background
  python game_auto.py kill             # end the game this tool launched
  python game_auto.py running          # print YES/NO
  python game_auto.py record <file>    # record clicks/keys into a script file
  python game_auto.py key <name> [hold_ms]
  python game_auto.py chord <mods+key>   e.g. alt+f1, ralt+8
  python game_auto.py click <x> <y>      absolute screen coords
  python game_auto.py rclick <x> <y>
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

Recorder notes:
  - Recording starts after a short countdown.
  - Press F9 to stop and save.
  - Left click records "click x y", right click records "rclick x y".
  - Common keys (escape, enter, arrows, tab, space, letters/digits) record as "key name".
"""
import atexit
import ctypes
import os
import sys
import time
import subprocess
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# VERIFY-INTEGRITY-3: do_click/do_rclick/do_move genuinely NEED to move the
# pointer -- that is this tool's whole job -- but they previously left it
# wherever the last scripted action dropped it, with no save/restore and no
# release of any ClipCursor the game had installed. So a one-line
# `game_auto.py click 800 600` permanently relocated the user's pointer.
#
# Fix shape (deliberately NOT "stop moving the cursor"): remember where the
# pointer was on entry, and put it back when the command/script finishes.
from smoke_lib.cursor_guard import (  # noqa: E402
    get_cursor_pos,
    set_cursor_pos,
    unclip_cursor,
)
from smoke_lib import nofocus  # noqa: E402

# Session cursor bookkeeping. `_CURSOR_MOVED` stays False for commands that
# never touch the pointer (launch/kill/key/wait/screenshot), so those restore
# nothing and cannot fight a user who is moving the mouse themselves.
_CURSOR_ORIGIN = None
_CURSOR_MOVED = False


def _cursor_session_begin() -> None:
    global _CURSOR_ORIGIN
    if _CURSOR_ORIGIN is None:
        _CURSOR_ORIGIN = get_cursor_pos()


def _cursor_session_end() -> None:
    """Release any game clamp and put the pointer back. Idempotent."""
    global _CURSOR_MOVED
    unclip_cursor()
    if _CURSOR_MOVED and _CURSOR_ORIGIN is not None:
        set_cursor_pos(_CURSOR_ORIGIN)
    _CURSOR_MOVED = False


def _warp(x: int, y: int) -> None:
    """Single chokepoint for every pointer warp -- records that we moved it.

    BACKGROUND-SMOKE-1: routed through nofocus.set_cursor_pos, which is a
    NO-OP unless MC2_HARNESS_ALLOW_FOREGROUND=1. Agent runs must never move
    the user's pointer; the restore path (cursor_guard) is exempt.
    """
    global _CURSOR_MOVED
    _cursor_session_begin()
    if not nofocus.set_cursor_pos(int(x), int(y)):
        return
    _CURSOR_MOVED = True

GAME_EXE = r"A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe"
GAME_DIR = r"A:\Games\mc2-opengl\mc2-win64-v0.4"
_PYAUTOGUI = None
KEYEVENTF_KEYUP = 0x0002
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010


# The pid of the game THIS tool launched. kill() ends only that process: an image-name kill would
# also end a game the owner or another lane is running.
PID_FILE = os.path.join(tempfile.gettempdir(), "game_auto.pid")


def _own_pid():
    try:
        with open(PID_FILE, "r", encoding="ascii") as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


def running() -> bool:
    pid = _own_pid()
    if pid is None:
        return False
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"PID eq {pid}", "/NH"],
            stderr=subprocess.DEVNULL,
            text=True, **nofocus.hidden_kwargs(),
        )
        return "mc2.exe" in out
    except Exception:
        return False


def launch():
    if running():
        print("already running")
        return
    # DETACHED_PROCESS (0x8) preserved; nofocus adds MC2_NO_CURSOR_GRAB=1 +
    # a non-activating STARTUPINFO so the window never takes focus.
    # NOFOCUS-FINISH-1: offscreen=False. This tool is the synthetic-input
    # driver -- its clicks target SCREEN coordinates and its `screenshot` verb
    # is pyautogui (the DESKTOP COMPOSITE), neither of which can reach a window
    # parked at (-32000,-32000). Declaring it makes nofocus REFUSE the launch
    # unless MC2_HARNESS_ALLOW_FOREGROUND=1, which is the correct outcome:
    # driving the game by synthetic input IS a user-present activity.
    proc = nofocus.spawn([GAME_EXE], cwd=GAME_DIR, creationflags=0x00000008,
                         offscreen=False)
    with open(PID_FILE, "w", encoding="ascii") as fh:
        fh.write(str(proc.pid))
    print("launched")


def kill():
    pid = _own_pid()
    if pid is None:
        print("nothing launched by this tool")
        return
    subprocess.run(["taskkill", "/F", "/PID", str(pid)], stderr=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, **nofocus.hidden_kwargs())
    os.remove(PID_FILE)
    print("killed")


def get_pyautogui():
    global _PYAUTOGUI
    if _PYAUTOGUI is None:
        import pyautogui

        pyautogui.FAILSAFE = False  # don't abort when mouse hits corner
        pyautogui.PAUSE = 0.05
        _PYAUTOGUI = pyautogui
    return _PYAUTOGUI


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

KEY_NAME_TO_VK = {
    "backspace": 0x08,
    "tab": 0x09,
    "enter": 0x0D,
    "return": 0x0D,
    "shift": 0x10,
    "ctrl": 0x11,
    "alt": 0x12,
    "escape": 0x1B,
    "esc": 0x1B,
    "space": 0x20,
    "pageup": 0x21,
    "pagedown": 0x22,
    "end": 0x23,
    "home": 0x24,
    "left": 0x25,
    "up": 0x26,
    "right": 0x27,
    "down": 0x28,
    "insert": 0x2D,
    "delete": 0x2E,
    "win": 0x5B,
    "lalt": 0xA4,
    "ralt": 0xA5,
    "altleft": 0xA4,
    "altright": 0xA5,
    "lshift": 0xA0,
    "rshift": 0xA1,
    "shiftleft": 0xA0,
    "shiftright": 0xA1,
}

for _digit in range(ord("0"), ord("9") + 1):
    KEY_NAME_TO_VK[chr(_digit).lower()] = _digit

for _letter in range(ord("A"), ord("Z") + 1):
    KEY_NAME_TO_VK[chr(_letter).lower()] = _letter


def native_key_down(name: str) -> bool:
    vk = KEY_NAME_TO_VK.get(name.lower())
    if vk is None:
        return False
    user32.keybd_event(vk, 0, 0, 0)
    return True


def native_key_up(name: str) -> bool:
    vk = KEY_NAME_TO_VK.get(name.lower())
    if vk is None:
        return False
    user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
    return True


def do_key(name: str, hold_ms: int = 50):
    # NOTE (BACKGROUND-SMOKE-1): keystrokes are left UNGUARDED, but they go to
    # whatever window has FOCUS. Under the no-focus contract the game window is
    # never activated, so it will not receive them -- a known limitation of the
    # menu-canary path, not something to "fix" by stealing focus.
    if native_key_down(name):
        time.sleep(hold_ms / 1000.0)
        native_key_up(name)
        return

    pyautogui = get_pyautogui()
    pyautogui.keyDown(name)
    time.sleep(hold_ms / 1000.0)
    pyautogui.keyUp(name)


def do_chord(spec: str):
    parts = spec.lower().split("+")
    mods = [MODS.get(p, p) for p in parts[:-1]]
    key = parts[-1]
    native_ok = True
    for m in mods:
        native_ok = native_key_down(m) and native_ok
    native_ok = native_key_down(key) and native_ok
    if native_ok:
        time.sleep(0.05)
        native_key_up(key)
        for m in reversed(mods):
            native_key_up(m)
        return

    pyautogui = get_pyautogui()
    for m in mods:
        pyautogui.keyDown(m)
    pyautogui.keyDown(key)
    time.sleep(0.05)
    pyautogui.keyUp(key)
    for m in reversed(mods):
        pyautogui.keyUp(m)


def do_click(x: int, y: int):
    # Synthetic clicks are meaningless when we may not own the pointer: the
    # warp is a no-op, so mouse_event would click wherever the USER's cursor
    # happens to be -- in his game. Skip entirely unless opted in.
    if not nofocus.allow_foreground():
        print("click skipped (background mode; MC2_HARNESS_ALLOW_FOREGROUND=1 to enable)")
        return
    _warp(x, y)
    time.sleep(0.03)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.03)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def do_rclick(x: int, y: int):
    if not nofocus.allow_foreground():
        print("rclick skipped (background mode; MC2_HARNESS_ALLOW_FOREGROUND=1 to enable)")
        return
    _warp(x, y)
    time.sleep(0.03)
    user32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
    time.sleep(0.03)
    user32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)


def do_move(x: int, y: int):
    _warp(x, y)


def do_screenshot(path: str):
    pyautogui = get_pyautogui()
    img = pyautogui.screenshot()
    img.save(path)
    print(f"saved {path}")


VK_NAMES = {
    0x08: "backspace",
    0x09: "tab",
    0x0D: "enter",
    0x1B: "escape",
    0x20: "space",
    0x21: "pageup",
    0x22: "pagedown",
    0x23: "end",
    0x24: "home",
    0x25: "left",
    0x26: "up",
    0x27: "right",
    0x28: "down",
    0x2D: "insert",
    0x2E: "delete",
}

for _digit in range(ord("0"), ord("9") + 1):
    VK_NAMES[_digit] = chr(_digit).lower()

for _letter in range(ord("A"), ord("Z") + 1):
    VK_NAMES[_letter] = chr(_letter).lower()

user32 = ctypes.windll.user32


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def vk_pressed(vk: int) -> bool:
    return bool(user32.GetAsyncKeyState(vk) & 0x8000)


def cursor_pos() -> tuple[int, int]:
    point = POINT()
    user32.GetCursorPos(ctypes.byref(point))
    return (point.x, point.y)


def flush_wait(out_lines: list[str], idle_seconds: float):
    if idle_seconds < 0.05:
        return
    out_lines.append(f"wait {idle_seconds:.2f}")


def record_script(filepath: str):
    target = Path(filepath)
    target.parent.mkdir(parents=True, exist_ok=True)
    print(f"Recording to {target}")
    print("Controls: F9 stop/save. Left click = click, right click = rclick.")
    print("Recording starts in 3 seconds...")
    time.sleep(3.0)

    last_event = time.monotonic()
    last_left = False
    last_right = False
    tracked_keys = sorted(VK_NAMES.keys())
    key_state = {vk: False for vk in tracked_keys}
    lines: list[str] = []

    while True:
        if vk_pressed(0x78):  # F9
            while vk_pressed(0x78):
                time.sleep(0.05)
            break

        left = vk_pressed(0x01)
        right = vk_pressed(0x02)

        if left and not last_left:
            now = time.monotonic()
            flush_wait(lines, now - last_event)
            x, y = cursor_pos()
            lines.append(f"click {x} {y}")
            print(lines[-1])
            last_event = now

        if right and not last_right:
            now = time.monotonic()
            flush_wait(lines, now - last_event)
            x, y = cursor_pos()
            lines.append(f"rclick {x} {y}")
            print(lines[-1])
            last_event = now

        last_left = left
        last_right = right

        for vk in tracked_keys:
            pressed = vk_pressed(vk)
            if pressed and not key_state[vk]:
                now = time.monotonic()
                flush_wait(lines, now - last_event)
                lines.append(f"key {VK_NAMES[vk]}")
                print(lines[-1])
                last_event = now
            key_state[vk] = pressed

        time.sleep(0.02)

    content = "\n".join(lines)
    if content:
        content += "\n"
    target.write_text(content)
    print(f"saved {target}")


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
    elif cmd == "record":
        record_script(args[0])
    elif cmd == "key":
        hold = int(args[1]) if len(args) > 1 else 50
        do_key(args[0], hold)
    elif cmd == "chord":
        do_chord(args[0])
    elif cmd == "click":
        do_click(int(args[0]), int(args[1]))
    elif cmd == "rclick":
        do_rclick(int(args[0]), int(args[1]))
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
    # VERIFY-INTEGRITY-3: the pointer goes back where the user left it even if
    # the scripted sequence raises or is Ctrl-C'd. atexit covers interpreter
    # teardown paths `finally` alone misses; _cursor_session_end is idempotent
    # and a no-op when no command actually warped the pointer.
    _cursor_session_begin()
    atexit.register(_cursor_session_end)
    try:
        dispatch(sys.argv[1], sys.argv[2:])
    finally:
        _cursor_session_end()

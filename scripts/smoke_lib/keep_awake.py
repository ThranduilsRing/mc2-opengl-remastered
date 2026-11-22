"""Keep the display and system awake while a harness runs the game.

A screensaver or display sleep that arms mid-run competes for the GPU and the present path, and the PC must not sleep during
a run. ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED holds for the calling thread until it exits, so the idle
timers do not fire during the run; it does not stop an already running screensaver (that is the owner's session). It did
NOT explain the 11-22 present stalls (260-790 ms per frame also without any screensaver; see W135-queue 11:44, likely
cross-adapter present with the display XTX disabled).
"""
import sys

ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001
ES_DISPLAY_REQUIRED = 0x00000002


def keep_display_on() -> bool:
    """True when Windows accepted the request; False elsewhere or on failure (never raises)."""
    if sys.platform != "win32":
        return False
    try:
        import ctypes
        prev = ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
        return prev != 0
    except Exception:
        return False

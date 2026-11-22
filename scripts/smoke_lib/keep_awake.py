"""Keep the display awake while a harness runs the game.

A screensaver that starts mid-run takes the GPU and stalls every present (perf-rerank-1121, 11-22: Ribbons.scr, frames
260-290 ms). ES_CONTINUOUS | ES_DISPLAY_REQUIRED holds for the calling thread until it exits, so the screensaver and
display sleep do not arm during the run; an already running screensaver is not stopped (that is the owner's session).
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

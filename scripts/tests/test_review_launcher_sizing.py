"""Exercise the four production RECT initializers against real Win32 sizing."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import re
import sys

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "tools/mc2_launcher/mc2_launcher.cpp"


@pytest.mark.skipif(sys.platform != "win32", reason="Win32 launcher")
@pytest.mark.parametrize("index,expected", enumerate([
    "0,0,clientW,clientH", "0,0,clientW,clientH", "0,0,w,h", "0,0,clientW,clientH",
]))
def test_launcher_client_size(index, expected):
    source = SOURCE.read_text(encoding="utf-8")
    initializers = re.findall(r"RECT rc = \{([^}]+)\};", source)
    assert len(initializers) == 4
    assert re.sub(r"\s+", "", initializers[index]) == expected
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.AdjustWindowRect.argtypes = [ctypes.POINTER(wintypes.RECT), wintypes.DWORD, wintypes.BOOL]
    user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
    user.CreateWindowExW.restype = wintypes.HWND
    user.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user.DestroyWindow.argtypes = [wintypes.HWND]
    # Same non-client styles as each production site; keep the probe window hidden.
    styles = [0x80C80000, 0x80C80000, 0x80C00000, 0x00CA0000]
    width, height = (360, 130) if index == 2 else (585, 600)
    rect = wintypes.RECT(0, 0, width, height)
    assert user.AdjustWindowRect(ctypes.byref(rect), styles[index], False)
    outer_w, outer_h = rect.right - rect.left, rect.bottom - rect.top
    assert outer_w > 0 and outer_h > 0
    hwnd = user.CreateWindowExW(0, "STATIC", "MC2 sizing regression", styles[index],
                                -30000, -30000, outer_w, outer_h, None, None, None, None)
    assert hwnd, ctypes.WinError(ctypes.get_last_error())
    try:
        client = wintypes.RECT()
        assert user.GetClientRect(hwnd, ctypes.byref(client))
        assert (client.right, client.bottom) == (width, height)
    finally:
        assert user.DestroyWindow(hwnd)

"""Repo-wide pytest setup: keep the harness cursor guard off the real desktop for every test.

WHY: tests that run the smoke runner against a fake child build a real CursorGuard, whose watchdog calls
ClipCursor(NULL) every 20 ms and restores the pointer, taking the mouse from whoever is using the machine
(owner incident 2026-10-10). MC2_CURSOR_GUARD_OFF=1 makes smoke_lib.win_input issue no win32 call.
The per-folder conftest.py files remain for runs that do not pick up this one.
"""
import os

os.environ["MC2_CURSOR_GUARD_OFF"] = "1"

#!/usr/bin/env python3
"""fg_trace.py LOGFILE SECONDS -- CMD...: 20ms foreground sampling; prints each foreground change with the number of lines
then present in LOGFILE and the last log line, to attribute an activation to a startup step."""
import ctypes, ctypes.wintypes as wt, subprocess, sys, time, os
u = ctypes.windll.user32
u.GetForegroundWindow.restype = wt.HWND
log, secs, cmd = sys.argv[1], float(sys.argv[2]), sys.argv[4:]
def fg():
    h = u.GetForegroundWindow()
    if not h: return "<none>"
    n = u.GetWindowTextLengthW(h); b = ctypes.create_unicode_buffer(n+1); u.GetWindowTextW(h, b, n+1)
    c = ctypes.create_unicode_buffer(128); u.GetClassNameW(h, c, 128)
    pid = wt.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return f"pid={pid.value} class={c.value!r} title={b.value!r}"
def lastline():
    try:
        L = open(log, errors="replace").read().splitlines()
        return len(L), (L[-1][:90] if L else "")
    except Exception: return 0, ""
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts"))
from smoke_lib import nofocus
p = nofocus.spawn(cmd, cwd=os.path.dirname(os.path.abspath(cmd[0]))); t0 = time.time(); prev = None
while time.time() - t0 < secs:
    s = fg()
    if s != prev:
        n, l = lastline(); print(f"t={time.time()-t0:6.2f} {s}  | log#{n}: {l}"); prev = s
    time.sleep(0.02)
p.wait()

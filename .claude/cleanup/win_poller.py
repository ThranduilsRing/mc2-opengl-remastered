#!/usr/bin/env python3
"""win_poller.py --root-exe NAME [--seconds N] -- CMD... : run CMD while, every 0.25s, enumerating ALL top-level visible
windows (EnumWindows + IsWindowVisible) whose owning PID is in the process tree rooted at any process named --root-exe
(descendants found via a Toolhelp snapshot each sample), plus the foreground window. Prints each distinct window once
with first/last time seen, minimized state and whether it was ever foreground. Exit 3 if any tree window was ever
non-minimized or foreground, or any non-GUI (console) window appeared."""
import argparse, ctypes, ctypes.wintypes as wt, os, subprocess, sys, threading, time
u = ctypes.windll.user32; k = ctypes.windll.kernel32
u.GetForegroundWindow.restype = wt.HWND
class PE(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]
k.CreateToolhelp32Snapshot.restype = wt.HANDLE
def procs():
    h = k.CreateToolhelp32Snapshot(2, 0); out = {}
    pe = PE(); pe.dwSize = ctypes.sizeof(pe)
    ok = k.Process32First(h, ctypes.byref(pe))
    while ok:
        out[pe.th32ProcessID] = (pe.th32ParentProcessID, pe.szExeFile.decode("mbcs", "replace"))
        ok = k.Process32Next(h, ctypes.byref(pe))
    k.CloseHandle(h); return out
def tree(root_exe):
    P = procs(); roots = {p for p, (_, e) in P.items() if e.lower() == root_exe.lower()}
    t = set(roots); changed = True
    while changed:
        changed = False
        for p, (pp, _) in P.items():
            if pp in t and p not in t: t.add(p); changed = True
    return t, P
ap = argparse.ArgumentParser(); ap.add_argument("--root-exe", required=True); ap.add_argument("--seconds", type=float, default=30)
ap.add_argument("cmd", nargs=argparse.REMAINDER); a = ap.parse_args()
cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
seen = {}; stop = threading.Event(); t0 = time.time(); samples = [0]
def poll():
    while not stop.is_set():
        T, P = tree(a.root_exe); fgw = u.GetForegroundWindow(); samples[0] += 1; now = round(time.time() - t0, 2)
        def cb(h, _):
            if not u.IsWindowVisible(h): return True
            pid = wt.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value not in T: return True
            n = u.GetWindowTextLengthW(h); b = ctypes.create_unicode_buffer(n + 1); u.GetWindowTextW(h, b, n + 1)
            c = ctypes.create_unicode_buffer(128); u.GetClassNameW(h, c, 128)
            key = (pid.value, P.get(pid.value, ("", "?"))[1], c.value, b.value)
            r = seen.setdefault(key, {"first": now, "last": now, "n": 0, "iconic": True, "fg": False})
            r["last"] = now; r["n"] += 1
            if not u.IsIconic(h): r["iconic"] = False
            if h == fgw: r["fg"] = True
            return True
        u.EnumWindows(WNDENUMPROC(cb), 0)
        time.sleep(0.25)
th = threading.Thread(target=poll, daemon=True); th.start()
p = subprocess.Popen(cmd)
while time.time() - t0 < a.seconds or p.poll() is None:
    if p.poll() is not None and time.time() - t0 >= a.seconds: break
    time.sleep(0.1)
stop.set(); th.join(2)
print(f"samples={samples[0]} over {round(time.time()-t0,1)}s; run rc={p.returncode}")
bad = False
for (pid, exe, cls, title), r in sorted(seen.items(), key=lambda kv: kv[1]["first"]):
    flag = (not r["iconic"]) or r["fg"] or cls == "ConsoleWindowClass"
    bad |= flag
    print(f"  {'!! ' if flag else '   '}first={r['first']:5}s last={r['last']:5}s n={r['n']:3} pid={pid} exe={exe!r} class={cls!r} title={title!r} minimized_always={r['iconic']} ever_foreground={r['fg']}")
if not seen: print("   (no visible top-level windows in the process tree)")
print("VISIBLE/FOREGROUND/CONSOLE WINDOW DETECTED" if bad else "OK: no non-minimized, foreground or console window ever appeared")
sys.exit(3 if bad else 0)

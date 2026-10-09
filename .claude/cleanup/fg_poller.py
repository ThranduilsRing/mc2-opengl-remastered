#!/usr/bin/env python3
"""fg_poller.py [--seconds N] -- CMD... : run CMD while sampling GetForegroundWindow() every 0.25s.

Prints the distinct (title, exe) foreground windows seen with counts and first-seen time, and exits 3 if any
foreground window belongs to a process whose image name matches --watch (default: Mission Editor/mc2.exe) or whose
title contains a --watch-title word. Samples are of the user's desktop; nothing is clicked."""
import argparse, ctypes, ctypes.wintypes as wt, subprocess, sys, threading, time, os
u = ctypes.windll.user32; k = ctypes.windll.kernel32
u.GetForegroundWindow.restype = wt.HWND
def fg():
    h = u.GetForegroundWindow()
    if not h: return ("<none>", "", "", 0)
    n = u.GetWindowTextLengthW(h)
    buf = ctypes.create_unicode_buffer(n + 1); u.GetWindowTextW(h, buf, n + 1)
    pid = wt.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(pid))
    exe = ""
    hp = k.OpenProcess(0x1000, False, pid.value)
    if hp:
        sz = wt.DWORD(520); b = ctypes.create_unicode_buffer(520)
        if k.QueryFullProcessImageNameW(hp, 0, b, ctypes.byref(sz)): exe = os.path.basename(b.value)
        k.CloseHandle(hp)
    cls = ctypes.create_unicode_buffer(256); u.GetClassNameW(h, cls, 256)
    return (buf.value, exe, cls.value, pid.value)
ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=float, default=30)
ap.add_argument("--watch", action="append", default=["mission editor.exe", "mc2.exe"])
ap.add_argument("--watch-title", action="append", default=["mission editor", "editorinterface"])
ap.add_argument("cmd", nargs=argparse.REMAINDER)
a = ap.parse_args()
cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
seen = {}; procinfo = {}; t0 = time.time(); stop = threading.Event(); samples = [0]
def poll():
    while not stop.is_set():
        s = fg(); samples[0] += 1
        first = s not in seen
        seen.setdefault(s, [0, round(time.time() - t0, 2)])[0] += 1
        if first and (s[1].lower() in [w.lower() for w in a.watch] or s[1].lower() == "cmd.exe"):
            try:
                procinfo[s] = subprocess.run(["powershell","-NoProfile","-Command",f"(Get-CimInstance Win32_Process -Filter 'ProcessId={s[3]}' | Select-Object ParentProcessId,CommandLine | Format-List | Out-String).Trim()"],capture_output=True,text=True,timeout=10).stdout.replace(chr(10)," ")
            except Exception as e: procinfo[s] = str(e)
        time.sleep(0.25)
th = threading.Thread(target=poll, daemon=True); th.start()
p = subprocess.Popen(cmd) if cmd else None
try:
    while time.time() - t0 < a.seconds and (p is None or p.poll() is None or time.time() - t0 < 5): time.sleep(0.1)
finally:
    stop.set(); th.join(2)
    if p and p.poll() is None: print("(command still running at window end; letting it finish)"); p.wait()
print(f"samples={samples[0]} over {a.seconds}s")
bad = False
for (title, exe, cls, pid), (n, first) in sorted(seen.items(), key=lambda kv: kv[1][1]):
    hit = exe.lower() in [w.lower() for w in a.watch] or any(w in title.lower() for w in a.watch_title)
    bad |= hit
    if (title, exe, cls, pid) in procinfo: print("       proc:", procinfo[(title, exe, cls, pid)][:300])
    print(f"  {'!! ' if hit else '   '}t={first:6}s n={n:4}  pid={pid} exe={exe!r} class={cls!r} title={title!r}")
print("EDITOR/GAME FOREGROUND DETECTED" if bad else "no editor/game window was ever foreground")
sys.exit(3 if bad else (p.returncode if p else 0))

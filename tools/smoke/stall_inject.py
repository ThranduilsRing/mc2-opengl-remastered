"""Host-stall injector for the mc2_10 intro-video floor proof.

Runs scripts/run_smoke.py --mission mc2_10 N times. Once the game's first debug-state dump appears (trigger=state, i.e.
the mission is playable; trigger=dll waits for the delay-loaded FFmpeg DLL), it suspends the process for --stall seconds, then resumes it. The sim-time quit passes
with the stall; a wall-clock quit fails with video_floor_missing.

The runner only writes the game log at the end of a run, so the trigger is a live signal, not the log.

usage: py -3 tools/smoke/stall_inject.py --exe EXE [-n 3] [--tag old|new] [--stall 11] [--delay 1.0]
Results append to tests/smoke/artifacts/stall_<tag>.txt (one line per run); exit 1 if any run was never stalled.
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

import psutil

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from console_child import child_kwargs  # noqa: E402

TRIGGER_DLL = "avcodec"


STATE = os.path.join(ROOT, "tests", "smoke", "artifacts")


def _state_mtime(install):
    try:
        return os.path.getmtime(os.path.join(install, "debug_state", "latest_render_state.json"))
    except OSError:
        return 0.0


def _loaded(pid):
    try:
        return any(TRIGGER_DLL in (m.path or "").lower() for m in psutil.Process(pid).memory_maps())
    except (psutil.Error, OSError):
        return False


def _suspend(pid, seconds):
    k = ctypes.WinDLL("kernel32")
    nt = ctypes.WinDLL("ntdll")
    k.OpenProcess.restype = ctypes.c_void_p
    h = k.OpenProcess(0x800, False, pid)  # PROCESS_SUSPEND_RESUME
    nt.NtSuspendProcess(ctypes.c_void_p(h))
    time.sleep(seconds)
    nt.NtResumeProcess(ctypes.c_void_p(h))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("-n", type=int, default=3)
    ap.add_argument("--tag", default="run")
    ap.add_argument("--stall", type=float, default=11.0)
    ap.add_argument("--delay", type=float, default=1.0)
    ap.add_argument("--mission", default="mc2_10")
    ap.add_argument("--trigger", choices=("state", "dll"), default="state")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    name = os.path.basename(exe).lower()
    env = dict(os.environ, MC2_SMOKE_REQUIRE_FINGERPRINT="0", MC2_DEBUG_STATE_DUMP="1")
    install = os.path.dirname(exe)
    outf = os.path.join(ROOT, "tests", "smoke", "artifacts", "stall_" + a.tag + ".txt")
    all_stalled = True
    for i in range(1, a.n + 1):
        p = subprocess.Popen([sys.executable, "scripts/run_smoke.py", "--mission", a.mission, "--keep-logs", "--exe", exe],
                             cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             **child_kwargs(redirected=True))
        t0 = time.time()
        stalled = False
        while p.poll() is None and time.time() - t0 < 400 and not stalled:
            time.sleep(0.25)
            if a.trigger == "state":
                hit = _state_mtime(install) > t0
            else:
                hit = any((pr.info["name"] or "").lower() == name and _loaded(pr.pid)
                          for pr in psutil.process_iter(["name"]))
            if hit:
                for pr in psutil.process_iter(["name"]):
                    if (pr.info["name"] or "").lower() == name:
                        print("trigger %s at age %.1fs" % (a.trigger, time.time() - t0), flush=True)
                        time.sleep(a.delay)
                        _suspend(pr.pid, a.stall)
                        stalled = True
                        break
        out = p.communicate()[0]
        m = re.search(r"artifacts: (.*)", out)
        d = m.group(1).strip() if m else ""
        log = open(os.path.join(d, a.mission + ".log"), errors="ignore").read() if d else ""
        res = "FAIL_video_floor_missing" if "video_floor_missing" in out else "PASS"
        all_stalled = all_stalled and stalled
        line = ("%s run=%d %s play_requested=%d sim_extend=%d stalled=%s rc=%s dir=%s\n"
                % (a.tag, i, res, log.count("event=play_requested"), log.count("sim_extend"), stalled, p.returncode, d))
        with open(outf, "a") as f:
            f.write(line)
        print(line, end="", flush=True)
    return 0 if all_stalled else 1


if __name__ == "__main__":
    sys.exit(main())

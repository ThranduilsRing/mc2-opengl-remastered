#!/usr/bin/env python3
"""Shared helpers for the RenderDoc automation CLIs (run under system python).

Responsibilities:
  * locate qrenderdoc.exe / renderdoccmd.exe (the headless entry points)
  * run rd_worker.py inside qrenderdoc with a job JSON, with a hard timeout
    and a PID-kill backstop (the worker os._exit's, but if a version quirk
    lets the UI open we must not leave a window hanging)
  * a tiny sequential game-run lane guard (own PID lockfile) so parallel
    agents don't launch mc2.exe on top of each other

No RenderDoc python module is imported here -- that only exists inside
qrenderdoc. This module just orchestrates processes.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_RDOC_DIR = Path(r"C:/Program Files/RenderDoc")
HERE = Path(__file__).resolve().parent
WORKER = HERE / "rd_worker.py"
LANE_LOCK = HERE / ".game-run.lock"


# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------
def find_tool(name: str, explicit: str | None = None) -> Path | None:
    """Locate a RenderDoc executable (qrenderdoc / renderdoccmd)."""
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    exe = name if name.endswith(".exe") else name + ".exe"
    from shutil import which
    on_path = which(name) or which(exe)
    if on_path:
        return Path(on_path)
    cand = DEFAULT_RDOC_DIR / exe
    if cand.is_file():
        return cand
    # RenderDoc may also live under Program Files (x86) or a versioned dir.
    for base in (Path(r"C:/Program Files"), Path(r"C:/Program Files (x86)")):
        if not base.is_dir():
            continue
        for sub in base.glob("RenderDoc*"):
            c = sub / exe
            if c.is_file():
                return c
    return None


def renderdoc_available() -> dict:
    """Report what RenderDoc entry points are present. Never raises."""
    q = find_tool("qrenderdoc")
    c = find_tool("renderdoccmd")
    return {
        "qrenderdoc": str(q) if q else None,
        "renderdoccmd": str(c) if c else None,
        "available": bool(q or c),
        "worker_capable": bool(q),   # replay analysis needs qrenderdoc --python
    }


# ---------------------------------------------------------------------------
# Worker execution (headless replay analysis)
# ---------------------------------------------------------------------------
def run_worker(job: dict, timeout: float = 180.0,
               qrenderdoc: str | None = None) -> dict:
    """Run rd_worker.py inside qrenderdoc with `job`; return parsed result.

    Writes the job to a temp file, points MC2_RD_JOB at it, launches
    qrenderdoc --python, waits (with a PID-kill backstop), then reads back
    job["result_json"].
    """
    q = find_tool("qrenderdoc", qrenderdoc)
    if not q:
        raise SystemExit(
            "qrenderdoc.exe not found. RenderDoc replay analysis needs the "
            "installed app (the stock Windows build ships no standalone "
            "renderdoc.pyd). Install RenderDoc or pass --qrenderdoc <path>.")
    if not WORKER.is_file():
        raise SystemExit("rd_worker.py missing next to _rdcommon.py")

    jf = tempfile.NamedTemporaryFile(
        "w", suffix=".rdjob.json", delete=False, encoding="utf-8")
    job_path = jf.name
    json.dump(job, jf)
    jf.close()

    env = os.environ.copy()
    env["MC2_RD_JOB"] = job_path
    # Keep RenderDoc from phoning home / checking updates during a headless run.
    env.setdefault("RENDERDOC_HEADLESS", "1")

    cmd = [str(q), "--python", str(WORKER)]
    err_path = job_path + ".err"
    for p in (job.get("result_json"), job.get("result_md"), err_path):
        try:
            if p and os.path.exists(p):
                os.remove(p)
        except OSError:
            pass

    # OWNER RULE: nothing an agent starts may take focus. qrenderdoc is a Qt GUI
    # (not the engine), so nofocus.spawn's engine env does not apply -- but its
    # non-activating, minimized STARTUPINFO does, and costs nothing for a
    # headless --python worker. Best effort: Qt may still raise its own window.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
    from smoke_lib import nofocus  # noqa: E402
    proc = subprocess.Popen(cmd, env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            **nofocus.popen_kwargs())  # background-launch: allow GUI tool, not the engine
    deadline = time.time() + timeout
    result_json = job["result_json"]
    try:
        while True:
            rc = proc.poll()
            if rc is not None:
                break
            # If the worker already wrote its result, the analysis is done even
            # if the process is lingering (UI opened on some versions).
            if os.path.exists(result_json):
                _terminate(proc)
                break
            if time.time() > deadline:
                _terminate(proc)
                raise SystemExit(
                    "qrenderdoc worker timed out after %ss (job %s)"
                    % (timeout, job_path))
            time.sleep(0.25)
    finally:
        _terminate(proc)

    if os.path.exists(err_path):
        with open(err_path) as fh:
            msg = fh.read()
        raise SystemExit("worker error:\n" + msg)
    if not os.path.exists(result_json):
        raise SystemExit(
            "worker produced no result (%s). Capture may be corrupt or the "
            "RenderDoc python API changed." % result_json)
    with open(result_json) as fh:
        res = json.load(fh)
    try:
        os.remove(job_path)
    except OSError:
        pass
    if not res.get("ok"):
        raise SystemExit("worker reported failure: %s"
                         % res.get("error", "<no detail>"))
    return res


def _terminate(proc):
    if proc.poll() is not None:
        return
    try:
        proc.terminate()
        for _ in range(20):
            if proc.poll() is not None:
                return
            time.sleep(0.1)
        proc.kill()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Sequential game-run lane guard (only for capture.py, which launches mc2.exe)
# ---------------------------------------------------------------------------
def acquire_game_lane(label: str, stale_after: float = 1800.0):
    """Best-effort exclusive lane for launching the game. Own-PID lockfile.

    Returns a release() callable. Raises SystemExit if another live run holds
    the lane. A stale lock (dead PID or older than stale_after) is reclaimed.
    """
    now = time.time()
    if LANE_LOCK.exists():
        try:
            info = json.loads(LANE_LOCK.read_text())
        except Exception:
            info = {}
        held_pid = info.get("pid")
        age = now - info.get("ts", 0)
        if held_pid and _pid_alive(held_pid) and age < stale_after:
            raise SystemExit(
                "game-run lane held by pid %s (%s) since %ss ago; RenderDoc "
                "captures run sequentially. Wait or clear %s."
                % (held_pid, info.get("label"), int(age), LANE_LOCK))
    LANE_LOCK.write_text(json.dumps(
        {"pid": os.getpid(), "label": label, "ts": now}))

    def release():
        try:
            if LANE_LOCK.exists():
                info = json.loads(LANE_LOCK.read_text())
                if info.get("pid") == os.getpid():
                    LANE_LOCK.unlink()
        except Exception:
            pass
    return release


def _pid_alive(pid: int) -> bool:
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "PID eq %d" % pid, "/NH"],
            capture_output=True, text=True, timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return str(pid) in out.stdout
    except Exception:
        return False

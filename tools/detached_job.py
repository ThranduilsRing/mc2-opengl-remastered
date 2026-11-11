#!/usr/bin/env python3
"""Detached background jobs that survive the launching (Claude) session, with a state file to re-attach to.

When the coordinator session died (2026-11-10 ~23:00) every background child died with it: a laptop gate run, a PC build, the quiet
window. A job started here runs under a small detached supervisor (DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, breakaway from the
parent's job object when allowed), which writes <jobs>/<name>.json (pid, cmd, log, start time, exit code on finish).

  py -3 tools/detached_job.py start [--name N] [--log P] [--timeout S] [--cwd D] -- <command...>   prints the job name
  py -3 tools/detached_job.py status NAME [--json]     running | done rc=N | lost (supervisor gone, no exit code); exit 0 only if done rc=0
  py -3 tools/detached_job.py wait NAME [--timeout S]  blocks, prints `PASS|FAIL|LOST rc=N secs=N log=P job=NAME`
  py -3 tools/detached_job.py run [--name N] ... -- <command...>   start-or-attach, then wait (what gate_wait.py and laptop_gate.py use)

`start`/`run` with a NAME whose job is still running re-attach instead of relaunching, so a resumed agent never starts a second copy.
State dir: G:/mc2-tmp/jobs (env MC2_JOBS_DIR overrides; used by the tests).
"""
import argparse
import json
import os
import subprocess
import sys
import time

DEFAULT_JOBS = "G:/mc2-tmp/jobs"
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_NO_WINDOW = 0x08000000
CREATE_BREAKAWAY_FROM_JOB = 0x01000000


def jobs_dir():
    return os.environ.get("MC2_JOBS_DIR") or DEFAULT_JOBS


def state_path(name):
    return os.path.join(jobs_dir(), name + ".json")


def read_state(name):
    try:
        with open(state_path(name), encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def write_state(st):
    path = state_path(st["name"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(st, fh, indent=1)
    os.replace(tmp, path)


def pid_alive(pid):
    if not pid:
        return False
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
    from smoke_lib.child_registry import pid_alive as _alive
    return _alive(int(pid))


def status(name):
    """-> state dict with `state` in starting|running|done|lost, or None for an unknown job."""
    st = read_state(name)
    if st is None:
        return None
    young = st.get("state") == "starting" and not st.get("supervisor_pid") and time.time() - st["started"] < 20
    if st.get("state") in ("starting", "running") and not young and not pid_alive(st.get("supervisor_pid")):
        st = dict(st, state="lost")   # the supervisor died without writing an exit code
    return st


def start(cmd, name=None, log=None, timeout=None, cwd=None):
    """Launch `cmd` under a detached supervisor. A still-running job of the same name is returned untouched (re-attach)."""
    name = name or f"job-{int(time.time())}-{os.getpid()}"
    cur = status(name)
    if cur and cur["state"] in ("starting", "running"):
        return cur
    log = log or os.path.join(jobs_dir(), name + ".log")
    os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
    st = {"name": name, "cmd": list(cmd), "cwd": cwd or os.getcwd(), "log": log, "timeout": timeout, "started": time.time(),
          "state": "starting", "supervisor_pid": None, "pid": None, "rc": None, "ended": None}
    write_state(st)
    argv = [sys.executable, os.path.abspath(__file__), "_run", state_path(name)]
    kw = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    if os.name == "nt":
        flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        try:
            # background-launch: allow the supervisor is this file under sys.executable with DETACHED_PROCESS (no window); never the game
            p = subprocess.Popen(argv, creationflags=flags | CREATE_BREAKAWAY_FROM_JOB, **kw)
        except OSError:   # the parent's job object forbids breakaway
            # background-launch: allow same detached supervisor without breakaway
            p = subprocess.Popen(argv, creationflags=flags, **kw)
    else:
        # background-launch: allow same detached supervisor (POSIX session)
        p = subprocess.Popen(argv, start_new_session=True, **kw)
    deadline = time.time() + 15
    while time.time() < deadline:
        cur = read_state(name) or st
        if cur.get("state") != "starting" or p.poll() is not None:
            break
        time.sleep(0.05)
    return status(name) or st


def wait(name, timeout=None, poll=0.5):
    """Block until the job is done or lost -> final status dict (state `running` if `timeout` elapsed first)."""
    t0 = time.time()
    while True:
        st = status(name)
        if st is None or st["state"] in ("done", "lost"):
            return st
        if timeout is not None and time.time() - t0 >= timeout:
            return st
        time.sleep(poll)


def verdict_line(st):
    if st["state"] == "done":
        secs = int((st.get("ended") or time.time()) - st["started"])
        return f"{'PASS' if st['rc'] == 0 else 'FAIL'} rc={st['rc']} secs={secs} log={st['log']} job={st['name']}"
    secs = int(time.time() - st["started"])
    return f"{'LOST' if st['state'] == 'lost' else 'RUNNING'} rc=None secs={secs} log={st['log']} job={st['name']}"


def run(cmd, name=None, log=None, timeout=None, cwd=None):
    """start-or-attach, then wait -> (rc, secs, state). rc is 124 on timeout, 125 when the job was lost."""
    st = start(cmd, name, log, timeout, cwd)
    st = wait(st["name"])
    secs = int(((st or {}).get("ended") or time.time()) - st["started"])
    if st["state"] == "done":
        return st["rc"], secs, st
    return 125, secs, st


def _kill_tree(pid):
    if os.name == "nt":
        # background-launch: allow runs in the detached supervisor (no console of its own); taskkill only
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=CREATE_NO_WINDOW)
    else:
        import signal
        try:
            os.killpg(int(pid), signal.SIGKILL)
        except OSError:
            pass


def supervise(path):
    """Supervisor body (runs detached): start the command, record its pid, wait, record the exit code."""
    with open(path, encoding="utf-8") as fh:
        st = json.load(fh)
    st["supervisor_pid"] = os.getpid()
    rc = 127
    try:
        flags = (CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP) if os.name == "nt" else 0
        with open(st["log"], "ab") as out:
            kw = dict(stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, cwd=st["cwd"])
            if os.name == "nt":
                kw["creationflags"] = flags
            else:
                kw["start_new_session"] = True
            # background-launch: allow runs the merge-window build/gate command a caller gave (gate_wait/laptop_gate), CREATE_NO_WINDOW, never the game
            p = subprocess.Popen(st["cmd"], **kw)
            st.update(pid=p.pid, state="running")
            write_state(st)
            try:
                rc = p.wait(timeout=st.get("timeout"))
            except subprocess.TimeoutExpired:
                _kill_tree(p.pid)
                p.wait()
                rc = 124
    except OSError as exc:
        with open(st["log"], "ab") as out:
            out.write(f"detached_job: cannot start {st['cmd']}: {exc}\n".encode())
    st.update(state="done", rc=rc, ended=time.time())
    write_state(st)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="verb", required=True)
    for verb in ("start", "run"):
        p = sub.add_parser(verb)
        p.add_argument("--name")
        p.add_argument("--log")
        p.add_argument("--timeout", type=float)
        p.add_argument("--cwd")
        p.add_argument("cmd", nargs=argparse.REMAINDER)
    p = sub.add_parser("status")
    p.add_argument("name")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("wait")
    p.add_argument("name")
    p.add_argument("--timeout", type=float)
    p = sub.add_parser("_run")
    p.add_argument("path")
    a = ap.parse_args(argv)
    if a.verb == "_run":
        return supervise(a.path)
    if a.verb in ("start", "run"):
        cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
        if not cmd:
            ap.error("give a command after --")
        if a.verb == "start":
            st = start(cmd, a.name, a.log, a.timeout, a.cwd)
            print(st["name"])
            return 0
        rc, _, st = run(cmd, a.name, a.log, a.timeout, a.cwd)
        print(verdict_line(st))
        return 0 if rc == 0 else 1
    if a.verb == "status":
        st = status(a.name)
        if st is None:
            print(f"unknown job {a.name}")
            return 2
        print(json.dumps(st) if a.json else verdict_line(st))
        return 0 if st["state"] == "done" and st["rc"] == 0 else 1
    st = wait(a.name, a.timeout)
    if st is None:
        print(f"unknown job {a.name}")
        return 2
    print(verdict_line(st))
    return 0 if st["state"] == "done" and st["rc"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

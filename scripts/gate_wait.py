"""One-verdict wait wrapper for merge-window builds and gates.

    py -3 scripts/gate_wait.py [--log PATH] [--timeout SEC] -- <command...>

Runs the command with all output going to the log (default <TMP>/gate_wait-<ts>.log) and prints exactly one
line when it ends: `PASS rc=0 secs=N log=PATH` or `FAIL rc=N secs=N log=PATH` (rc=124 on timeout; the child
tree is killed). Launch it with the Bash tool's run_in_background so the tool timeout never sees it; the
completion notice then carries the single line. Exit code mirrors the verdict.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402


def resolve_cmd(cmd):
    """A bare bash/sh from a Python child on Windows is the WSL launcher (no `py` inside): use Git's bash."""
    if cmd and os.name == "nt" and os.path.basename(cmd[0]).lower() in ("bash", "bash.exe", "sh", "sh.exe"):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
        from lane_merge import git_bash
        return [git_bash()] + list(cmd[1:])
    return cmd


def wait(cmd, log, timeout=None):
    cmd = resolve_cmd(cmd)
    started = time.time()
    with open(log, "wb") as fh:
        p = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **console_child.child_kwargs(redirected=True))
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True, **console_child.child_kwargs(redirected=True))
            else:
                p.kill()
            p.wait()
            rc = 124
    return rc, int(time.time() - started)


LOG_DIR = "G:/mc2-tmp"  # never $TMP: it can be C: (lane rules, meta-0220 P2#6)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log")
    ap.add_argument("--timeout", type=float)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("give a command after --")
    log = a.log or os.path.join(LOG_DIR, f"gate_wait-{int(time.time())}.log")
    os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
    try:
        rc, secs = wait(cmd, log, a.timeout)
    except OSError as exc:
        print(f"FAIL rc=127 secs=0 log={log} ({exc})")
        return 127
    print(f"{'PASS' if rc == 0 else 'FAIL'} rc={rc} secs={secs} log={log}")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

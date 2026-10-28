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


def wait(cmd, log, timeout=None):
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log")
    ap.add_argument("--timeout", type=float)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
    if not cmd:
        ap.error("give a command after --")
    log = a.log or os.path.join(os.environ.get("TMP", "G:/mc2-tmp"), f"gate_wait-{int(time.time())}.log")
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

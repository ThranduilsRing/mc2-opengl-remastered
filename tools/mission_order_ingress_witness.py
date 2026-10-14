"""Mission ingress witness through the real tick hooks and the leased lane3 smoke harness.

Run after the orchestrator commits, builds and deploys the reviewed executable.
Exit 1 means the witness failed. The drain/retirement negative-control executables
must each produce that failure; this tool never manually drains or retires orders.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools/dev_shell"))
import console_child
from mc2_cmd import send_command


def witness(smoke, send, now=time.monotonic, sleep=time.sleep, startup_timeout=3720,
            runtime_timeout=25):
    """Three automatically consumed batches cross the 8192-receipt capacity."""
    started = now()
    deadline = started + startup_timeout
    last_report = None
    next_report = started

    def request(command, batch, attempt=0):
        nonlocal last_report, next_report
        try:
            reply = send(command)
            if not isinstance(reply, dict) or type(reply.get("ok")) is not bool or reply.get("version") != 1:
                raise ValueError(f"invalid dev-shell v1 reply: {reply!r}")
            report = json.dumps(reply, sort_keys=True)
        except (OSError, ValueError) as error:
            reply = None
            report = f"{type(error).__name__}: {error}"
        identity = (command, batch, report)
        if attempt or identity != last_report or now() >= next_report:
            print(f"mission ingress elapsed={now() - started:.2f}s batch={batch} "
                  f"attempt={attempt} command={command} response={report}", flush=True)
            last_report = identity
            next_report = now() + 30
        return reply

    # Lease waits and mission loading precede the listener; neither spends the play budget.
    while smoke.poll() is None and now() < deadline:
        reply = request("ping", 0)
        if reply is not None:
            if not reply["ok"]:
                return False
            break
        sleep(0.25)
    else:
        print("mission ingress readiness failed: harness exited or startup deadline expired", flush=True)
        return False

    deadline = now() + runtime_timeout
    for batch in range(3):
        attempt = 0
        while smoke.poll() is None and now() < deadline:
            attempt += 1
            reply = request("order_ingress_fixture_submit", batch + 1, attempt)
            # A lost admission reply is ambiguous: replay could submit the next batch twice.
            if reply is None:
                print("mission ingress submission ambiguous: not replaying", flush=True)
                return False
            if reply["ok"]:
                break
            sleep(min(0.25, max(0, deadline - now())))
        else:
            print(f"mission ingress batch {batch + 1}/3 submission failed: "
                  "harness exited or play deadline expired", flush=True)
            return False
        while smoke.poll() is None and now() < deadline:
            reply = request("order_ingress_fixture_check", batch + 1)
            if reply is not None and reply["ok"] and now() < deadline:
                break
            sleep(0.25)
        else:
            print(f"mission ingress batch {batch + 1}/3 check failed: "
                  "harness exited or play deadline expired", flush=True)
            return False
        print(f"mission ingress batch {batch + 1}/3 installed and automatically retired", flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19874)
    args = parser.parse_args()
    env = dict(os.environ, MC2_DEV_SHELL="1", MC2_DEV_SHELL_PORT=str(args.port),
               MC2_SMOKE_REQUIRE_FINGERPRINT="1")
    command = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--mission", "mc2_01",
               "--duration", "30", "--keep-logs", "--deploy", "lane3"]
    with subprocess.Popen(command, cwd=ROOT, env=env, **console_child.child_kwargs()) as smoke:
        passed = witness(smoke, lambda command: send_command(
            command, port=args.port, timeout=10 if command.endswith("_submit") else 1),
            startup_timeout=float(env.get("MC2_HOST_LEASE_TIMEOUT") or 3600) + 120)
        try:
            status = smoke.wait(timeout=120)
        except subprocess.TimeoutExpired:
            smoke.terminate()
            smoke.wait(timeout=15)
            print("mission ingress runtime harness TIMEOUT")
            return 124
    print(f"mission ingress runtime witness passed={passed} smoke={status}")
    return 0 if passed and status == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

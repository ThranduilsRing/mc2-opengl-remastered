"""Mission ingress witness through the real tick hooks and the leased lane3 smoke harness.

Run after the orchestrator commits, builds and deploys the reviewed executable.
Exit 1 means the witness failed. The drain/retirement negative-control executables
must each produce that failure; this tool never manually drains or retires orders.
"""
import argparse
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


def witness(smoke, send, now=time.monotonic, sleep=time.sleep):
    """Three automatically consumed batches cross the 8192-receipt capacity."""
    for batch in range(3):
        deadline = None if batch == 0 else now() + 10
        while smoke.poll() is None and (deadline is None or now() < deadline):
            try:
                if send("order_ingress_fixture_submit").get("ok"):
                    break
            except (OSError, ValueError):
                pass
            sleep(0.25)
        else:
            return False
        deadline = now() + 10
        while smoke.poll() is None and now() < deadline:
            try:
                if send("order_ingress_fixture_check").get("ok"):
                    break
            except (OSError, ValueError):
                pass
            sleep(0.25)
        else:
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
        passed = witness(smoke, lambda command: send_command(command, port=args.port, timeout=10))
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

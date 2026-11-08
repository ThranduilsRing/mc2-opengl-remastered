"""Issue a real UI jump and require the mover's execution receipt (headless)."""
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child


def main():
    exe, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    channel = output / "commands.txt"
    ack = output / "commands.txt.ack"
    channel.write_text("skip_intro\nlist player\n", encoding="utf-8")
    ack.unlink(missing_ok=True)
    env = dict(os.environ, MC2_UI_COMMAND_FILE=str(channel), MC2_DIAG_TAGS="JUMP_ORDER")
    command = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--exe", str(exe),
               "--mission", "mc2_01", "--headless", "--duration", "30", "--keep-logs"]
    done = threading.Event()
    with (output / "runner.log").open("w") as log:
        proc = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                **console_child.child_kwargs(redirected=True))
        try:
            def read_until(marker, start=0):
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline and proc.poll() is None:
                    text = ack.read_text(errors="replace") if ack.exists() else ""
                    if marker in text[start:]:
                        return text[start:]
                    done.wait(0.05)
                raise RuntimeError(f"missing ack: {marker}")

            def send(line):
                start = ack.stat().st_size
                channel.write_text(line + "\n", encoding="utf-8")
                return read_until(line + " ->", start)

            roster = read_until("list player -> ok")
            ids = re.findall(r"\bid=(\d+) name=", roster)
            candidate = None
            for pid in ids:
                detail = send("unit " + pid)
                if "jump: capable=true" in detail:
                    pos = re.search(r"pos=\(([-.\d]+),([-.\d]+),", detail)
                    candidate = pid, float(pos[1]), float(pos[2])
                    break
            if not candidate:
                raise RuntimeError("mc2_01 has no jump-capable player mover")
            pid, x, y = candidate
            send("select " + pid)
            for dx, dy in [(128, 0), (-128, 0), (0, 128), (0, -128)]:
                reply = send(f"order jump {x + dx} {y + dy}")
                if "-> ok" in reply:
                    break
            else:
                raise RuntimeError("no nearby jump target accepted")
            rc = proc.wait(timeout=210)
            evidence = (output / "runner.log").read_text(errors="replace")
            artifacts = re.findall(r"\[runner\] artifacts: (.+)", evidence)
            engine = Path(artifacts[-1].strip()) / "mc2_01.log" if artifacts else None
            engine_text = engine.read_text(errors="replace") if engine and engine.exists() else ""
            (output / "engine.log").write_text(engine_text, encoding="utf-8")
            receipt = f"[JUMP_ORDER v1] event=executing part={pid} "
            if rc or receipt not in engine_text:
                raise RuntimeError(f"jump not executed: rc={rc}; expected {receipt}")
            print(f"PASS jump command executed by part={pid}; evidence={output}")
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=30)
            channel.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
